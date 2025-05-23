// C++
#include <cmath>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <unistd.h> // for unlink()

// Qt
#include <QList>
#include <QMap>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QStringList>
#include <QWaitCondition>
#include <QtAlgorithms>

// MythTV
#include "libmythbase/mythconfig.h"

#include "libmyth/audio/audiooutput.h"
#include "libmythbase/exitcodes.h"
#include "libmythbase/mthreadpool.h"
#include "libmythbase/mythcorecontext.h"
#include "libmythbase/mythdbcon.h"
#include "libmythbase/mythlogging.h"
#include "libmythbase/programinfo.h"
#include "libmythtv/HLS/httplivestream.h"
#include "libmythtv/deletemap.h"
#include "libmythtv/io/mythavformatwriter.h"
#include "libmythtv/jobqueue.h"
#include "libmythtv/mythavutil.h"
#include "libmythtv/recordingprofile.h"
#include "libmythtv/tvremoteutil.h"

// MythTranscode
#include "audioreencodebuffer.h"
#include "cutter.h"
#include "mythtranscodeplayer.h"
#include "transcode.h"
#include "videodecodebuffer.h"

extern "C" {
#include "libavcodec/avcodec.h"
#include "libswscale/swscale.h"
}

#define LOC QString("Transcode: ")

Transcode::Transcode(ProgramInfo *pginfo) :
    m_proginfo(pginfo),
    m_recProfile(new RecordingProfile("Transcoders"))
{
}

Transcode::~Transcode()
{
    SetPlayerContext(nullptr);
    delete m_outBuffer;
    delete m_fifow;
    delete m_recProfile;
}

bool Transcode::GetProfile(const QString& profileName, const QString& encodingType,
                           int height, int frameRate)
{
    if (profileName.toLower() == "autodetect")
    {
        if (height == 1088)
            height = 1080;

        QString autoProfileName = QObject::tr("Autodetect from %1").arg(height);
        if (frameRate == 25 || frameRate == 30)
            autoProfileName += "i";
        if (frameRate == 50 || frameRate == 60)
            autoProfileName += "p";

        bool result = false;
        LOG(VB_GENERAL, LOG_NOTICE,
            QString("Transcode: Looking for autodetect profile: %1")
                .arg(autoProfileName));
        result = m_recProfile->loadByGroup(autoProfileName, "Transcoders");

        if (!result && encodingType == "MPEG-2")
        {
            result = m_recProfile->loadByGroup("MPEG2", "Transcoders");
            autoProfileName = "MPEG2";
        }
        if (!result && (encodingType == "MPEG-4" || encodingType == "RTjpeg"))
        {
            result = m_recProfile->loadByGroup("RTjpeg/MPEG4",
                                         "Transcoders");
            autoProfileName = "RTjpeg/MPEG4";
        }
        if (!result)
        {
            LOG(VB_GENERAL, LOG_ERR,
                QString("Transcode: Couldn't find profile for : %1")
                    .arg(encodingType));

            return false;
        }

        LOG(VB_GENERAL, LOG_NOTICE,
            QString("Transcode: Using autodetect profile: %1")
                .arg(autoProfileName));
    }
    else
    {
        bool isNum = false;
        int profileID = profileName.toInt(&isNum);
        // If a bad profile is specified, there will be trouble
        if (isNum && profileID > 0)
            m_recProfile->loadByID(profileID);
        else if (!m_recProfile->loadByGroup(profileName, "Transcoders"))
        {
            LOG(VB_GENERAL, LOG_ERR, QString("Couldn't find profile #: %1")
                    .arg(profileName));
            return false;
        }
    }
    return true;
}

void Transcode::SetPlayerContext(PlayerContext *player_ctx)
{
    if (player_ctx == m_ctx)
        return;

    delete m_ctx;
    m_ctx = player_ctx;
}

int Transcode::TranscodeFile(const QString &inputname,
                             const QString &outputname,
                             [[maybe_unused]] const QString &profileName,
                             bool honorCutList, bool framecontrol,
                             int jobID, const QString& fifodir,
                             bool fifo_info, bool cleanCut,
                             frm_dir_map_t &deleteMap,
                             int AudioTrackNo,
                             bool passthru)
{
    QDateTime curtime = MythDate::current();
    QDateTime statustime = curtime;
    int audioFrame = 0;
    std::unique_ptr<Cutter> cutter = nullptr;
    std::unique_ptr<MythAVFormatWriter> avfw = nullptr;
    std::unique_ptr<MythAVFormatWriter> avfw2 = nullptr;
    std::unique_ptr<HTTPLiveStream> hls = nullptr;
    int hlsSegmentSize = 0;
    int hlsSegmentFrames = 0;

    if (jobID >= 0)
        JobQueue::ChangeJobComment(jobID, "0% " + QObject::tr("Completed"));

    if (m_hlsMode)
    {
        m_avfMode = true;

        if (m_hlsStreamID != -1)
        {
            hls = std::make_unique<HTTPLiveStream>(m_hlsStreamID);
            hls->UpdateStatus(kHLSStatusStarting);
            hls->UpdateStatusMessage("Transcoding Starting");
            m_cmdWidth = hls->GetWidth();
            m_cmdHeight = hls->GetHeight();
            m_cmdBitrate = hls->GetBitrate();
            m_cmdAudioBitrate = hls->GetAudioBitrate();
        }
    }

    if (!m_avfMode)
    {
        LOG(VB_GENERAL, LOG_ERR,
            "AVFormat mode not set.");
        return REENCODE_ERROR;
    }

    // Input setup
    auto *player_ctx = new PlayerContext(kTranscoderInUseID);
    player_ctx->SetPlayingInfo(m_proginfo);
    MythMediaBuffer *rb = (hls && (m_hlsStreamID != -1)) ?
        MythMediaBuffer::Create(hls->GetSourceFile(), false, false) :
        MythMediaBuffer::Create(inputname, false, false);
    if (!rb || !rb->GetLastError().isEmpty())
    {
        LOG(VB_GENERAL, LOG_ERR,
            QString("Transcoding aborted, error: '%1'")
            .arg(rb? rb->GetLastError() : ""));
        delete player_ctx;
        return REENCODE_ERROR;
    }
    player_ctx->SetRingBuffer(rb);
    player_ctx->SetPlayer(new MythTranscodePlayer(player_ctx, static_cast<PlayerFlags>(kVideoIsNull | kNoITV)));
    SetPlayerContext(player_ctx);
    auto * player = dynamic_cast<MythTranscodePlayer*>(GetPlayer());
    if (player == nullptr)
    {
        LOG(VB_GENERAL, LOG_ERR,
            QString("Transcoding aborted, failed to retrieve MythPlayer object"));
        return REENCODE_ERROR;
    }
    if (m_proginfo->GetRecordingEndTime() > curtime)
    {
        player_ctx->SetRecorder(RemoteGetExistingRecorder(m_proginfo));
        player->SetWatchingRecording(true);
    }

    if (m_showProgress)
    {
        statustime = statustime.addSecs(5);
    }

    AudioOutput *audioOutput = new AudioReencodeBuffer(FORMAT_NONE, 0,
                                                       passthru);
    AudioReencodeBuffer *arb = ((AudioReencodeBuffer*)audioOutput);
    player->GetAudio()->SetAudioOutput(audioOutput);
    player->SetTranscoding(true);

    if (player->OpenFile() < 0)
    {
        LOG(VB_GENERAL, LOG_ERR, "Transcoding aborted, error opening file.");
        SetPlayerContext(nullptr);
        return REENCODE_ERROR;
    }

    if (AudioTrackNo > -1)
    {
        LOG(VB_GENERAL, LOG_INFO,
            QString("Set audiotrack number to %1").arg(AudioTrackNo));
        player->GetDecoder()->SetTrack(kTrackTypeAudio, AudioTrackNo);
    }

    long long total_frame_count = player->GetTotalFrameCount();
    long long new_frame_count = total_frame_count;
    if (honorCutList && m_proginfo)
    {
        LOG(VB_GENERAL, LOG_INFO, "Honoring the cutlist while transcoding");

        frm_dir_map_t::const_iterator it;
        QString cutStr;
        long long lastStart = 0;

        if (deleteMap.empty())
            m_proginfo->QueryCutList(deleteMap);

        for (it = deleteMap.cbegin(); it != deleteMap.cend(); ++it)
        {
            if (*it)
            {
                if (!cutStr.isEmpty())
                    cutStr += ",";
                cutStr += QString("%1-").arg((long)it.key());
                lastStart = it.key();
            }
            else
            {
                if (cutStr.isEmpty())
                    cutStr += "0-";
                cutStr += QString("%1").arg((long)it.key());
                new_frame_count -= (it.key() - lastStart);
            }
        }
        if (cutStr.isEmpty())
            cutStr = "Is Empty";
        else if (cutStr.endsWith('-') && (total_frame_count > lastStart))
        {
            new_frame_count -= (total_frame_count - lastStart);
            cutStr += QString("%1").arg(total_frame_count);
        }
        LOG(VB_GENERAL, LOG_INFO, QString("Cutlist        : %1").arg(cutStr));
        LOG(VB_GENERAL, LOG_INFO, QString("Original Length: %1 frames")
                .arg((long)total_frame_count));
        LOG(VB_GENERAL, LOG_INFO, QString("New Length     : %1 frames")
                .arg((long)new_frame_count));

        if ((m_proginfo->QueryIsEditing()) ||
            (JobQueue::IsJobRunning(JOB_COMMFLAG, *m_proginfo)))
        {
            LOG(VB_GENERAL, LOG_INFO, "Transcoding aborted, cutlist changed");
            SetPlayerContext(nullptr);
            return REENCODE_CUTLIST_CHANGE;
        }
        m_proginfo->ClearMarkupFlag(MARK_UPDATED_CUT);
        curtime = curtime.addSecs(60);
    }

    player->GetAudio()->ReinitAudio();

    QString vidsetting = nullptr;

    QSize buf_size = player->GetVideoBufferSize();
    int video_width = buf_size.width();
    int video_height = buf_size.height();

    if (video_height == 1088) {
       LOG(VB_GENERAL, LOG_NOTICE,
           "Found video height of 1088.  This is unusual and "
           "more than likely the video is actually 1080 so mythtranscode "
           "will treat it as such.");
    }

    DecoderBase* dec = player->GetDecoder();
    float video_aspect = dec ? dec->GetVideoAspect() : 4.0F / 3.0F;
    float video_frame_rate = player->GetFrameRate();
    int newWidth = video_width;
    int newHeight = video_height;
    bool halfFramerate = false;
    bool skippedLastFrame = false;

    if (m_avfMode)
    {
        newWidth = m_cmdWidth;
        newHeight = m_cmdHeight;

        // Absolutely no purpose is served by scaling video up beyond it's
        // original resolution, quality is degraded, transcoding is
        // slower and in future we may wish to scale bitrate according to
        // resolution, so it would also waste bandwidth (when streaming)
        //
        // This change could be said to apply for all transcoding, but for now
        // we're limiting it to HLS where it's uncontroversial
        if (m_hlsMode)
        {
//             if (newWidth > video_width)
//                 newWidth = video_width;
            if (newHeight > video_height)
            {
                newHeight = video_height;
                newWidth = 0;
            }
        }

        // TODO: is this necessary?  It got commented out, but may still be
        // needed.
        // int actualHeight = (video_height == 1088 ? 1080 : video_height);

        // If height or width are 0, then we need to calculate them
        if (newHeight == 0 && newWidth > 0)
            newHeight = (int)(1.0F * newWidth / video_aspect);
        else if (newWidth == 0 && newHeight > 0)
            newWidth = (int)(1.0F * newHeight * video_aspect);
        else if (newWidth == 0 && newHeight == 0)
        {
            newHeight = 480;
            newWidth = (int)(1.0F * 480 * video_aspect);
            if (newWidth > 640)
            {
                newWidth = 640;
                newHeight = (int)(1.0F * 640 / video_aspect);
            }
        }

        // make sure dimensions are valid for MPEG codecs
        newHeight = (newHeight + 15) & ~0xF;
        newWidth  = (newWidth  + 15) & ~0xF;

        avfw = std::make_unique<MythAVFormatWriter>();
        if (!avfw)
        {
            LOG(VB_GENERAL, LOG_ERR,
                "Transcoding aborted, error creating AVFormatWriter.");
            SetPlayerContext(nullptr);
            return REENCODE_ERROR;
        }

        avfw->SetVideoBitrate(m_cmdBitrate);
        avfw->SetHeight(newHeight);
        avfw->SetWidth(newWidth);
        avfw->SetAspect(video_aspect);
        avfw->SetAudioBitrate(m_cmdAudioBitrate);
        avfw->SetAudioChannels(arb->m_channels);
        avfw->SetAudioFrameRate(arb->m_eff_audiorate);
        avfw->SetAudioFormat(FORMAT_S16);

        if (m_hlsMode)
        {

            if (m_hlsStreamID == -1)
            {
                hls = std::make_unique<HTTPLiveStream>(inputname, newWidth, newHeight,
                                         m_cmdBitrate, m_cmdAudioBitrate,
                                         m_hlsMaxSegments, 0, 0);

                m_hlsStreamID = hls->GetStreamID();
                if (!hls || m_hlsStreamID == -1)
                {
                    LOG(VB_GENERAL, LOG_ERR, "Unable to create new stream");
                    SetPlayerContext(nullptr);
                    return REENCODE_ERROR;
                }
            }

            int segmentSize = hls->GetSegmentSize();

            LOG(VB_GENERAL, LOG_NOTICE,
                QString("HLS: Using segment size of %1 seconds")
                    .arg(segmentSize));

            if (!m_hlsDisableAudioOnly)
            {
                int audioOnlyBitrate = hls->GetAudioOnlyBitrate();

                avfw2 = std::make_unique<MythAVFormatWriter>();
                avfw2->SetContainer("mpegts");
                avfw2->SetAudioCodec("aac");
                avfw2->SetAudioBitrate(audioOnlyBitrate);
                avfw2->SetAudioChannels(arb->m_channels);
                avfw2->SetAudioFrameRate(arb->m_eff_audiorate);
                avfw2->SetAudioFormat(FORMAT_S16);
            }

            avfw->SetContainer("mpegts");
            avfw->SetVideoCodec("libx264");
            avfw->SetAudioCodec("aac");
            hls->UpdateStatus(kHLSStatusStarting);
            hls->UpdateStatusMessage("Transcoding Starting");
            hls->UpdateSizeInfo(newWidth, newHeight, video_width, video_height);

            if (!hls->InitForWrite())
            {
                LOG(VB_GENERAL, LOG_ERR, "hls->InitForWrite() failed");
                SetPlayerContext(nullptr);
                return REENCODE_ERROR;
            }

            if (video_frame_rate > 30)
            {
                halfFramerate = true;
                avfw->SetFramerate(video_frame_rate/2);

                if (avfw2)
                    avfw2->SetFramerate(video_frame_rate/2);

                hlsSegmentSize = (int)(segmentSize * video_frame_rate / 2);
            }
            else
            {
                avfw->SetFramerate(video_frame_rate);

                if (avfw2)
                    avfw2->SetFramerate(video_frame_rate);

                hlsSegmentSize = (int)(segmentSize * video_frame_rate);
            }

            avfw->SetKeyFrameDist(30);
            if (avfw2)
                avfw2->SetKeyFrameDist(30);

            hls->AddSegment();
            avfw->SetFilename(hls->GetCurrentFilename());
            if (avfw2)
                avfw2->SetFilename(hls->GetCurrentFilename(true));
        }
        else
        {
            avfw->SetContainer(m_cmdContainer);
            avfw->SetVideoCodec(m_cmdVideoCodec);
            avfw->SetAudioCodec(m_cmdAudioCodec);
            avfw->SetFilename(outputname);
            avfw->SetFramerate(video_frame_rate);
            avfw->SetKeyFrameDist(30);
        }

        int threads    = gCoreContext->GetNumSetting("HTTPLiveStreamThreads", 2);
        QString preset = gCoreContext->GetSetting("HTTPLiveStreamPreset", "veryfast");
        QString tune   = gCoreContext->GetSetting("HTTPLiveStreamTune", "film");

        LOG(VB_GENERAL, LOG_NOTICE,
            QString("x264 HLS using: %1 threads, '%2' profile and '%3' tune")
                .arg(QString::number(threads), preset, tune));

        avfw->SetThreadCount(threads);
        avfw->SetEncodingPreset(preset);
        avfw->SetEncodingTune(tune);

        if (avfw2)
            avfw2->SetThreadCount(1);

        if (!avfw->Init())
        {
            LOG(VB_GENERAL, LOG_ERR, "avfw->Init() failed");
            SetPlayerContext(nullptr);
            return REENCODE_ERROR;
        }

        if (!avfw->OpenFile())
        {
            LOG(VB_GENERAL, LOG_ERR, "avfw->OpenFile() failed");
            SetPlayerContext(nullptr);
            return REENCODE_ERROR;
        }

        if (avfw2 && !avfw2->Init())
        {
            LOG(VB_GENERAL, LOG_ERR, "avfw2->Init() failed");
            SetPlayerContext(nullptr);
            return REENCODE_ERROR;
        }

        if (avfw2 && !avfw2->OpenFile())
        {
            LOG(VB_GENERAL, LOG_ERR, "avfw2->OpenFile() failed");
            SetPlayerContext(nullptr);
            return REENCODE_ERROR;
        }

        arb->m_audioFrameSize = avfw->GetAudioFrameSize() * arb->m_channels * 2;
    }

    if (honorCutList && !deleteMap.empty())
    {
        if (cleanCut)
        {
            // Have the player seek only part of the way
            // through a cut, and then use the cutter to
            // discard the rest
            cutter = std::make_unique<Cutter>();
            cutter->SetCutList(deleteMap, m_ctx);
            player->SetCutList(cutter->AdjustedCutList());
        }
        else
        {
            // Have the player apply the cut list
            player->SetCutList(deleteMap);
        }
    }

    player->InitForTranscode();
    if (player->IsErrored())
    {
        LOG(VB_GENERAL, LOG_ERR,
            "Unable to initialize MythPlayer for Transcode");
        SetPlayerContext(nullptr);
        return REENCODE_ERROR;
    }

    // must come after InitForTranscode - which creates the VideoOutput instance
    if (m_hlsMode && player->GetVideoOutput())
        player->GetVideoOutput()->SetDeinterlacing(true, false, DEINT_CPU | DEINT_MEDIUM);

    MythVideoFrame frame;
    // Do not use padding when compressing to RTjpeg or when in fifomode.
    // The RTjpeg compressor doesn't know how to handle strides different to
    // video width.
    bool nonAligned = vidsetting == "RTjpeg" || !fifodir.isEmpty(); 
    bool rescale = (video_width != newWidth) || (video_height != newHeight) || nonAligned;

    if (rescale)
    {
        if (nonAligned)
        {
            // Set a stride identical to actual width, to ease fifo post-conversion process.
            // 1080i/p video is actually 1088 because of the 16x16 blocks so
            // we have to fudge the output size here.  nuvexport knows how to handle
            // this and as of right now it is the only app that uses the fifo ability.
            size_t newSize = MythVideoFrame::GetBufferSize(FMT_YV12,
                video_width, video_height == 1080 ? 1088 : video_height, 0 /* aligned */);
            uint8_t* newbuffer = MythVideoFrame::GetAlignedBuffer(newSize);
            if (!newbuffer)
                return REENCODE_ERROR;
            frame.Init(FMT_YV12, newbuffer, newSize, video_width, video_height, nullptr, 0);
        }
        else
        {
            frame.Init(FMT_YV12, newWidth, newHeight);
        }
    }

    if (!fifodir.isEmpty())
    {
        AudioPlayer *aplayer = player->GetAudio();
        const char  *audio_codec_name {nullptr};

        switch(aplayer->GetCodec())
        {
            case AV_CODEC_ID_AC3:
                audio_codec_name = "ac3";
                break;
            case AV_CODEC_ID_EAC3:
                audio_codec_name = "eac3";
                break;
            case AV_CODEC_ID_DTS:
                audio_codec_name = "dts";
                break;
            case AV_CODEC_ID_TRUEHD:
                audio_codec_name = "truehd";
                break;
            case AV_CODEC_ID_MP3:
                audio_codec_name = "mp3";
                break;
            case AV_CODEC_ID_MP2:
                audio_codec_name = "mp2";
                break;
            case AV_CODEC_ID_AAC:
                audio_codec_name = "aac";
                break;
            case AV_CODEC_ID_AAC_LATM:
                audio_codec_name = "aac_latm";
                break;
            default:
                audio_codec_name = "unknown";
        }

        if (!arb->m_passthru)
            audio_codec_name = "raw";

        // If cutlist is used then get info on first uncut frame
        if (honorCutList && fifo_info)
        {
            bool is_key = false;
            int did_ff = 0;
            player->TranscodeGetNextFrame(did_ff, is_key, true);

            QSize buf_size2 = player->GetVideoBufferSize();
            video_width = buf_size2.width();
            video_height = buf_size2.height();
            video_aspect = player->GetVideoAspect();
            video_frame_rate = player->GetFrameRate();
        }

        // Display details of the format of the fifo data.
        LOG(VB_GENERAL, LOG_INFO,
            QString("FifoVideoWidth %1").arg(video_width));
        LOG(VB_GENERAL, LOG_INFO,
            QString("FifoVideoHeight %1").arg(video_height));
        LOG(VB_GENERAL, LOG_INFO,
            QString("FifoVideoAspectRatio %1").arg(video_aspect));
        LOG(VB_GENERAL, LOG_INFO,
            QString("FifoVideoFrameRate %1").arg(video_frame_rate));
        LOG(VB_GENERAL, LOG_INFO,
            QString("FifoAudioFormat %1").arg(audio_codec_name));
        LOG(VB_GENERAL, LOG_INFO,
            QString("FifoAudioChannels %1").arg(arb->m_channels));
        LOG(VB_GENERAL, LOG_INFO,
            QString("FifoAudioSampleRate %1").arg(arb->m_eff_audiorate));

        if(fifo_info)
        {
            // Request was for just the format of fifo data, not for
            // the actual transcode, so stop here.
            unlink(outputname.toLocal8Bit().constData());
            SetPlayerContext(nullptr);
            return REENCODE_OK;
        }

        QString audfifo = fifodir + QString("/audout");
        QString vidfifo = fifodir + QString("/vidout");
        int audio_size = arb->m_eff_audiorate * arb->m_bytes_per_frame;
        // framecontrol is true if we want to enforce fifo sync.
        if (framecontrol)
            LOG(VB_GENERAL, LOG_INFO, "Enforcing sync on fifos");
        m_fifow = new MythFIFOWriter(2, framecontrol);

        if (!m_fifow->FIFOInit(0, QString("video"), vidfifo, frame.m_bufferSize, 50) ||
            !m_fifow->FIFOInit(1, QString("audio"), audfifo, audio_size, 25))
        {
            LOG(VB_GENERAL, LOG_ERR,
                "Error initializing fifo writer.  Aborting");
            unlink(outputname.toLocal8Bit().constData());
            SetPlayerContext(nullptr);
            return REENCODE_ERROR;
        }
        LOG(VB_GENERAL, LOG_INFO,
            QString("Video %1x%2@%3fps Audio rate: %4")
                .arg(video_width).arg(video_height)
                .arg(video_frame_rate)
                .arg(arb->m_eff_audiorate));
        LOG(VB_GENERAL, LOG_INFO, "Created fifos. Waiting for connection.");
    }

    frm_dir_map_t::iterator dm_iter;

    int did_ff = 0;

    long curFrameNum = 0;
    frame.m_frameNumber = 1;
    long totalAudio = 0;
    int dropvideo = 0;
    // timecode of the last read video frame in input time
    std::chrono::milliseconds lasttimecode = 0ms;
    // timecode of the last write video frame in input or output time
    std::chrono::milliseconds lastWrittenTime = 0ms;
    // delta between the same video frame in input and output due to applying the cut list
    std::chrono::milliseconds timecodeOffset = 0ms;

    float rateTimeConv = arb->m_eff_audiorate / 1000.0F;
    float vidFrameTime = 1000.0F / video_frame_rate;
    auto  vidFrameTimeMs = millisecondsFromFloat(vidFrameTime);
    int wait_recover = 0;
    MythVideoOutput *videoOutput = player->GetVideoOutput();
    bool is_key = false;
    AVFrame imageIn;
    AVFrame imageOut;
    struct SwsContext  *scontext = nullptr;

    if (m_fifow)
        LOG(VB_GENERAL, LOG_INFO, "Dumping Video and Audio data to fifos");
    else if (m_hlsMode)
        LOG(VB_GENERAL, LOG_INFO, "Transcoding for HTTP Live Streaming");
    else if (m_avfMode)
        LOG(VB_GENERAL, LOG_INFO, "Transcoding to libavformat container");
    else
        LOG(VB_GENERAL, LOG_INFO, "Transcoding Video and Audio");

    auto *videoBuffer =
        new VideoDecodeBuffer(player, videoOutput, honorCutList);
    MThreadPool::globalInstance()->start(videoBuffer, "VideoDecodeBuffer");

    QElapsedTimer flagTime;
    flagTime.start();

    if (cutter)
        cutter->Activate(vidFrameTime * rateTimeConv, total_frame_count);

    bool stopSignalled = false;
    MythVideoFrame *lastDecode = nullptr;

    if (hls)
    {
        hls->UpdateStatus(kHLSStatusRunning);
        hls->UpdateStatusMessage("Transcoding");
    }

    while ((!stopSignalled) &&
           (lastDecode = videoBuffer->GetFrame(did_ff, is_key)))
    {
        float new_aspect = lastDecode->m_aspect;

        if (cutter)
            cutter->NewFrame(lastDecode->m_frameNumber);

// frame timecode is on input time base
        frame.m_timecode = lastDecode->m_timecode;

        // if the timecode jumps backwards just use the last frame's timecode plus the duration of a frame
        if (frame.m_timecode < lasttimecode)
            frame.m_timecode = lasttimecode + vidFrameTimeMs;

        if (m_fifow)
        {
            MythAVUtil::FillAVFrame(&imageIn, lastDecode);
            MythAVUtil::FillAVFrame(&imageOut, &frame);

            scontext = sws_getCachedContext(scontext,
                           lastDecode->m_width, lastDecode->m_height, MythAVUtil::FrameTypeToPixelFormat(lastDecode->m_type),
                           frame.m_width, frame.m_height, MythAVUtil::FrameTypeToPixelFormat(frame.m_type),
                           SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
            // Typically, wee aren't rescaling per say, we're just correcting the stride set by the decoder.
            // However, it allows to properly handle recordings that see their resolution change half-way.
            sws_scale(scontext, imageIn.data, imageIn.linesize, 0,
                      lastDecode->m_height, imageOut.data, imageOut.linesize);

            totalAudio += arb->GetSamples(frame.m_timecode);
            std::chrono::milliseconds audbufTime = millisecondsFromFloat(totalAudio / rateTimeConv);
            std::chrono::milliseconds auddelta = frame.m_timecode - audbufTime;
            std::chrono::milliseconds vidTime = millisecondsFromFloat(curFrameNum * vidFrameTime);
            std::chrono::milliseconds viddelta = frame.m_timecode - vidTime;
            std::chrono::milliseconds delta = viddelta - auddelta;
            std::chrono::milliseconds absdelta = std::chrono::abs(delta);
            if (absdelta < 500ms && absdelta >= vidFrameTimeMs)
            {
               QString msg = QString("Audio is %1ms %2 video at # %3: "
                                     "auddelta=%4, viddelta=%5")
                   .arg(absdelta.count())
                   .arg(((delta > 0ms) ? "ahead of" : "behind"))
                   .arg((int)curFrameNum)
                   .arg(auddelta.count())
                   .arg(viddelta.count());
                LOG(VB_GENERAL, LOG_INFO, msg);
                dropvideo = (delta > 0ms) ? 1 : -1;
                wait_recover = 0;
            }
            else if (delta >= 500ms && delta < 10s)
            {
                if (wait_recover == 0)
                {
                    dropvideo = 5;
                    wait_recover = 6;
                }
                else if (wait_recover == 1)
                {
                    // Video is badly lagging.  Try to catch up.
                    int count = 0;
                    while (delta > vidFrameTimeMs)
                    {
                        if (!cutter || !cutter->InhibitDummyFrame())
                            m_fifow->FIFOWrite(0, frame.m_buffer, frame.m_bufferSize);

                        count++;
                        delta -= vidFrameTimeMs;
                    }
                    QString msg = QString("Added %1 blank video frames")
                                  .arg(count);
                    LOG(VB_GENERAL, LOG_INFO, msg);
                    curFrameNum += count;
                    dropvideo = 0;
                    wait_recover = 0;
                }
                else
                {
                    wait_recover--;
                }
            }
            else
            {
                dropvideo = 0;
                wait_recover = 0;
            }

#if 0
            int buflen = (int)(arb->audiobuffer_len / rateTimeConv);
            LOG(VB_GENERAL, LOG_DEBUG,
                QString("%1: video time: %2 audio time: %3 "
                        "buf: %4 exp: %5 delta: %6")
                    .arg(curFrameNum) .arg(frame.m_timecode.count())
                    .arg(arb->last_audiotime) .arg(buflen) .arg(audbufTime.count())
                    .arg(delta.count()));
#endif
            AudioBuffer *ab = nullptr;
            while ((ab = arb->GetData(frame.m_timecode)) != nullptr)
            {
                if (!cutter ||
                    !cutter->InhibitUseAudioFrames(ab->m_frames, &totalAudio))
                    m_fifow->FIFOWrite(1, ab->data(), ab->size());

                delete ab;
            }

            if (dropvideo < 0)
            {
                if (cutter && cutter->InhibitDropFrame())
                    m_fifow->FIFOWrite(0, frame.m_buffer, frame.m_bufferSize);

                LOG(VB_GENERAL, LOG_INFO, "Dropping video frame");
                dropvideo++;
                curFrameNum--;
            }
            else
            {
                if (!cutter || !cutter->InhibitUseVideoFrame())
                    m_fifow->FIFOWrite(0, frame.m_buffer, frame.m_bufferSize);

                if (dropvideo)
                {
                    if (!cutter || !cutter->InhibitDummyFrame())
                        m_fifow->FIFOWrite(0, frame.m_buffer, frame.m_bufferSize);

                    curFrameNum++;
                    dropvideo--;
                }
            }
            videoOutput->DoneDisplayingFrame(lastDecode);
            player->GetCC608Reader()->FlushTxtBuffers();
            lasttimecode = frame.m_timecode;
        }
        else
        {
            if (did_ff == 1)
            {
                did_ff = 2;
                timecodeOffset += (frame.m_timecode - lasttimecode -
                                   millisecondsFromFloat(vidFrameTime));
            }

            if (video_aspect != new_aspect)
            {
                video_aspect = new_aspect;
            }


            QSize buf_size4 = player->GetVideoBufferSize();

            if (video_width != buf_size4.width() ||
                video_height != buf_size4.height())
            {
                video_width = buf_size4.width();
                video_height = buf_size4.height();

                LOG(VB_GENERAL, LOG_INFO,
                    QString("Resizing from %1x%2 to %3x%4")
                        .arg(video_width).arg(video_height)
                        .arg(newWidth).arg(newHeight));
            }

            if (rescale)
            {
                MythAVUtil::FillAVFrame(&imageIn, lastDecode);
                MythAVUtil::FillAVFrame(&imageOut, &frame);

                int bottomBand = (lastDecode->m_height == 1088) ? 8 : 0;
                scontext = sws_getCachedContext(scontext,
                               lastDecode->m_width, lastDecode->m_height, MythAVUtil::FrameTypeToPixelFormat(lastDecode->m_type),
                               frame.m_width, frame.m_height, MythAVUtil::FrameTypeToPixelFormat(frame.m_type),
                               SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);

                sws_scale(scontext, imageIn.data, imageIn.linesize, 0,
                          lastDecode->m_height - bottomBand,
                          imageOut.data, imageOut.linesize);
            }

            // audio is fully decoded, so we need to reencode it
            AudioBuffer *ab = nullptr;
            while ((ab = arb->GetData(lastWrittenTime)) != nullptr)
            {
                auto *buf = (unsigned char *)ab->data();
                if (m_avfMode)
                {
                    if (did_ff != 1)
                    {
                        std::chrono::milliseconds tc = ab->m_time - timecodeOffset;
                        avfw->WriteAudioFrame(buf, audioFrame, tc);

                        if (avfw2)
                        {
                            if ((avfw2->GetTimecodeOffset() == -1ms) &&
                                (avfw->GetTimecodeOffset() != -1ms))
                            {
                                avfw2->SetTimecodeOffset(
                                    avfw->GetTimecodeOffset());
                            }

                            tc = ab->m_time - timecodeOffset;
                            avfw2->WriteAudioFrame(buf, audioFrame, tc);
                        }

                        ++audioFrame;
                    }
                }
                delete ab;
            }

            if (!m_avfMode)
            {
                LOG(VB_GENERAL, LOG_ERR,
                    "AVFormat mode not set.");
                return REENCODE_ERROR;
            }
            lasttimecode = frame.m_timecode;
            frame.m_timecode -= timecodeOffset;

            if (m_avfMode)
            {
                if (halfFramerate && !skippedLastFrame)
                {
                    skippedLastFrame = true;
                }
                else
                {
                    skippedLastFrame = false;

                    if ((hls) &&
                        (avfw->GetFramesWritten()) &&
                        (hlsSegmentFrames > hlsSegmentSize) &&
                        (avfw->NextFrameIsKeyFrame()))
                    {
                        hls->AddSegment();
                        avfw->ReOpen(hls->GetCurrentFilename());

                        if (avfw2)
                            avfw2->ReOpen(hls->GetCurrentFilename(true));

                        hlsSegmentFrames = 0;
                    }

                    if (avfw->WriteVideoFrame(rescale ? &frame : lastDecode) > 0)
                    {
                        lastWrittenTime = frame.m_timecode + timecodeOffset;
                        if (hls)
                            ++hlsSegmentFrames;
                    }

                }
            }
        }
        if (MythDate::current() > statustime)
        {
            if (m_showProgress)
            {
                LOG(VB_GENERAL, LOG_INFO,
                    QString("Processed: %1 of %2 frames(%3 seconds)").
                        arg(curFrameNum).arg((long)total_frame_count).
                        arg((long)(curFrameNum / video_frame_rate)));
            }

            if (hls && hls->CheckStop())
            {
                hls->UpdateStatus(kHLSStatusStopping);
                stopSignalled = true;
            }

            statustime = MythDate::current().addSecs(5);
        }
        if (MythDate::current() > curtime)
        {
            if (honorCutList && m_proginfo && !m_avfMode &&
                m_proginfo->QueryMarkupFlag(MARK_UPDATED_CUT))
            {
                LOG(VB_GENERAL, LOG_NOTICE,
                    "Transcoding aborted, cutlist updated");

                unlink(outputname.toLocal8Bit().constData());
                SetPlayerContext(nullptr);
                if (videoBuffer)
                    videoBuffer->stop();
                return REENCODE_CUTLIST_CHANGE;
            }

            if ((jobID >= 0) || (VERBOSE_LEVEL_CHECK(VB_GENERAL, LOG_INFO)))
            {
                if (JobQueue::GetJobCmd(jobID) == JOB_STOP)
                {
                    LOG(VB_GENERAL, LOG_NOTICE,
                        "Transcoding STOPped by JobQueue");

                    unlink(outputname.toLocal8Bit().constData());
                    SetPlayerContext(nullptr);
                    if (videoBuffer)
                        videoBuffer->stop();
                    if (hls)
                    {
                        hls->UpdateStatus(kHLSStatusStopped);
                        hls->UpdateStatusMessage("Transcoding Stopped");
                    }
                    return REENCODE_STOPPED;
                }

                float flagFPS = 0.0;
                float elapsed = flagTime.elapsed() / 1000.0F;
                if (elapsed != 0.0F)
                    flagFPS = curFrameNum / elapsed;

                total_frame_count = player->GetCurrentFrameCount();
                int percentage = curFrameNum * 100 / total_frame_count;

                if (hls)
                    hls->UpdatePercentComplete(percentage);

                if (jobID >= 0)
                {
                    JobQueue::ChangeJobComment(jobID,
                              QObject::tr("%1% Completed @ %2 fps.")
                                          .arg(percentage).arg(flagFPS));
                }
                else
                {
                    LOG(VB_GENERAL, LOG_INFO,
                        QString("mythtranscode: %1% Completed @ %2 fps.")
                            .arg(percentage).arg(flagFPS));
                }

            }
            curtime = MythDate::current().addSecs(20);
        }

        curFrameNum++;
        frame.m_frameNumber = 1 + (curFrameNum << 1);

        player->DiscardVideoFrame(lastDecode);
    }

    sws_freeContext(scontext);

    if (!m_fifow)
    {
        if (avfw)
            avfw->CloseFile();

        if (avfw2)
            avfw2->CloseFile();

        if (!m_avfMode && m_proginfo)
        {
            m_proginfo->ClearPositionMap(MARK_KEYFRAME);
            m_proginfo->ClearPositionMap(MARK_GOP_START);
            m_proginfo->ClearPositionMap(MARK_GOP_BYFRAME);
            m_proginfo->ClearPositionMap(MARK_DURATION_MS);
        }
    } else {
        m_fifow->FIFODrain();
    }

    if (hls)
    {
        if (!stopSignalled)
        {
            hls->UpdateStatus(kHLSStatusCompleted);
            hls->UpdateStatusMessage("Transcoding Completed");
            hls->UpdatePercentComplete(100);
        }
        else
        {
            hls->UpdateStatus(kHLSStatusStopped);
            hls->UpdateStatusMessage("Transcoding Stopped");
        }
    }

    if (videoBuffer)
        videoBuffer->stop();

    SetPlayerContext(nullptr);

    return REENCODE_OK;
}

/* vim: set expandtab tabstop=4 shiftwidth=4: */

