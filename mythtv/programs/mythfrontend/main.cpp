#include <qapplication.h>
#include <qsqldatabase.h>
#include <unistd.h>

#include "guidegrid.h"
#include "tv.h"
#include "settings.h"
#include "menubox.h"
#include "scheduler.h"
#include "infostructs.h"
#include "programinfo.h"
#include "playbackbox.h"
#include "deletebox.h"
#include "viewscheduled.h"

int startGuide(int startchannel)
{
    GuideGrid gg(startchannel);

    gg.exec();

    int chan = gg.getLastChannel();
    return chan;
}

int startManaged(TV *tv, QString prefix)
{
    QSqlDatabase *db = QSqlDatabase::database();
    ViewScheduled vsb(prefix, tv, db);

    vsb.Show();
    vsb.exec();

    return 0;
}

int startPlayback(TV *tv, QString prefix)
{
    QSqlDatabase *db = QSqlDatabase::database();  
    PlaybackBox pbb(prefix, tv, db);

    pbb.Show();

    pbb.exec();

    return 0;
}

int startDelete(TV *tv, QString prefix)
{
    QSqlDatabase *db = QSqlDatabase::database();
    DeleteBox delbox(prefix, tv, db);
   
    delbox.Show();
    
    delbox.exec();

    return 0;
}

void startTV(TV *tv)
{
    tv->LiveTV();
}

void startRecording(TV *tv, ProgramInfo *rec)
{
    ProgramInfo *tvrec = new ProgramInfo(*rec);
    tv->StartRecording(tvrec);
}

int askRecording(TV *tv, ProgramInfo *rec, int timeuntil)
{
    ProgramInfo *tvrec = new ProgramInfo(*rec);
    int retval = tv->AllowRecording(tvrec, timeuntil);

    delete tvrec;
    return retval;
}

void *runScheduler(void *dummy)
{
    TV *tv = (TV *)dummy;
    QSqlDatabase *db = QSqlDatabase::database("SUBDB");

    Scheduler *sched = new Scheduler(db);

    sched->FillRecordLists();

    int secsleft = -1;
    int asksecs = -1;
    bool asked = false;

    ProgramInfo *nextRecording = sched->GetNextRecording();
    QDateTime nextrectime;
    if (nextRecording)
    {
        nextrectime = nextRecording->startts;
        asked = false;
    }
    QDateTime curtime = QDateTime::currentDateTime();

    QDateTime lastupdate = curtime;

    while (1)
    {
        sleep(1);

        if (sched->CheckForChanges() ||
            (lastupdate.date().day() != curtime.date().day()))
        {
            lastupdate = curtime;
            sched->FillRecordLists();
            nextRecording = sched->GetNextRecording();
            if (nextRecording)
            {
                nextrectime = nextRecording->startts;
                asked = false;
            }
        }

        curtime = QDateTime::currentDateTime();
        if (nextRecording)
        {
            secsleft = curtime.secsTo(nextrectime);
            asksecs = secsleft - 30;

            if (tv->GetState() == kState_WatchingLiveTV && asksecs <= 0)
            {
                if (!asked)
                {
                    asked = true;
                    int result = askRecording(tv, nextRecording, secsleft);

                    if (result == 3)
                    {
                        sched->RemoveFirstRecording();
                        nextRecording = sched->GetNextRecording();
                    }
 
                    if (nextRecording)
                    {
                        nextrectime = nextRecording->startts;
                        curtime = QDateTime::currentDateTime();
                        secsleft = curtime.secsTo(nextrectime);
                    }
                }
            }
            if (secsleft <= -2)
            {
                // don't record stuff that's already started..
                if (secsleft > -30)
                    startRecording(tv, nextRecording);

                sched->RemoveFirstRecording();
                nextRecording = sched->GetNextRecording();

                if (nextRecording)
                {
                   nextrectime = nextRecording->startts;
                   curtime = QDateTime::currentDateTime();
                   secsleft = curtime.secsTo(nextrectime);
                }
            }
         //   else 
         //       cout << secsleft << " secs left until " << nextRecording->title << endl;
        }
    }
    
    return NULL;
}

int main(int argc, char **argv)
{
    QApplication a(argc, argv);

    QSqlDatabase *db = QSqlDatabase::addDatabase("QMYSQL3");
    if (!db)
    {
        printf("Couldn't connect to database\n");
        return -1;
    }
    db->setDatabaseName("mythconverg");
    db->setUserName("mythtv");
    db->setPassword("mythtv");
    db->setHostName("localhost");

    QSqlDatabase *subthread = QSqlDatabase::addDatabase("QMYSQL3", "SUBDB");
    if (!subthread)
    {
        printf("Couldn't connect to database\n");
        return -1;
    }
    subthread->setDatabaseName("mythconverg");
    subthread->setUserName("mythtv");
    subthread->setPassword("mythtv");
    subthread->setHostName("localhost");    

    if (!db->open() || !subthread->open())
    {
        printf("couldn't open db\n");
        return -1;
    }

    TV *tv = new TV("3");

    QString prefix = tv->GetFilePrefix();
 
    pthread_t scthread;
    pthread_create(&scthread, NULL, runScheduler, tv);

    while (1)
    {
//        MenuBox *maindiag = new MenuBox("");
//
//        maindiag->AddButton("TV");
//        maindiag->AddButton("Music");

//        maindiag->Show();
//        int result = maindiag->exec();

//        if (result == 1)
//        {
//    while (1)
//    {
        MenuBox *diag = new MenuBox("MythTV");

        diag->AddButton("Watch TV");
        diag->AddButton("Schedule a Recording");
        diag->AddButton("Fix Recording Conflicts");
        diag->AddButton("Watch a Recording");  
        diag->AddButton("Delete Recordings");
 
        diag->Show();
        int result = diag->exec();

        switch (result)
        {
            case 1: startTV(tv); break;
            case 2: startGuide(3); break;
            case 3: startManaged(tv, prefix); break;
            case 4: startPlayback(tv, prefix); break;
            case 5: startDelete(tv, prefix); break;
            default: break;
        }

        delete diag;
//        if (result == 0)
//            break;
//    }
//        }
//        else if (result == 2)
//        {
//            system("mythmusic /root/music/");
//        }
//    
//        delete maindiag;
    }

    delete tv;

    return 0;
}
