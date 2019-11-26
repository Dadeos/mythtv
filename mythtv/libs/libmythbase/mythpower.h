#ifndef MYTHPOWER_H
#define MYTHPOWER_H

// Qt
#include <QMutex>
#include <QObject>
#include <QDateTime>
#include <QTimer>

// MythTV
#include "mythbaseexp.h"
#include "referencecounter.h"

// Seconds
#define DEFAULT_SHUTDOWN_WAIT 5
#define MAXIMUM_SHUTDOWN_WAIT 30

class MBASE_PUBLIC MythPower : public QObject, public ReferenceCounter
{
    Q_OBJECT

  public:
    enum PowerLevel
    {
        UPS           = -2,
        ACPower       = -1,
        BatteryEmpty  = 0,
        BatteryLow    = 10,
        BatteryFull   = 100,
        UnknownPower  = 101,
        Unset
    };

    enum Feature
    {
        FeatureNone        = 0x00,
        FeatureShutdown    = 0x01,
        FeatureSuspend     = 0x02,
        FeatureHibernate   = 0x04,
        FeatureRestart     = 0x08,
        FeatureHybridSleep = 0x10
    };

    Q_DECLARE_FLAGS(Features, Feature)

    static MythPower* AcquireRelease(void* Reference, bool Acquire, uint MinimumDelay = 0);
    virtual bool RequestFeature    (Feature Request, bool Delay = true);
    Features     GetFeatures       (void);
    bool         IsFeatureSupported(Feature Supported);
    int          GetPowerLevel     (void);

  public slots:
    virtual void CancelFeature     (void);

  signals:
    void ShuttingDown   (void);
    void Suspending     (void);
    void Hibernating    (void);
    void Restarting     (void);
    void HybridSleeping (void);
    void WillShutDown   (uint MilliSeconds = 0);
    void WillSuspend    (uint MilliSeconds = 0);
    void WillHibernate  (uint MilliSeconds = 0);
    void WillRestart    (uint MilliSeconds = 0);
    void WillHybridSleep(uint MilliSeconds = 0);
    void WokeUp         (qint64 SecondsAsleep);
    void LowBattery     (void);

  protected slots:
    void FeatureTimeout  (void);
    virtual void Refresh (void) {  }

  protected:
    static QMutex s_lock;

    MythPower();
    virtual ~MythPower();

    virtual void   Init              (void);
    virtual bool   DoFeature         (bool = false) { return false; }
    virtual void   DidWakeUp         (void);
    virtual void   FeatureHappening  (void);
    virtual bool   ScheduleFeature   (enum Feature Type, uint Delay);
    void           SetRequestedDelay (uint Delay);
    void           PowerLevelChanged (int Level);
    static QString FeatureToString   (enum Feature Type);
    bool           FeatureIsEquivalent(Feature First, Feature Second);

    Features  m_features             { FeatureNone };
    Feature   m_scheduledFeature     { FeatureNone };
    bool      m_isSpontaneous        { false };
    uint      m_maxRequestedDelay    { 0 };
    uint      m_maxSupportedDelay    { MAXIMUM_SHUTDOWN_WAIT };
    QTimer    m_featureTimer         { };
    QDateTime m_sleepTime            { };
    int       m_powerLevel           { Unset };
    bool      m_warnForLowBattery    { false };

  private:
    Q_DISABLE_COPY(MythPower)
};

Q_DECLARE_OPERATORS_FOR_FLAGS(MythPower::Features)

#endif // MYTHPOWER_H
