#ifndef BBATTERY_STORE_H
#define BBATTERY_STORE_H
#include "measurement.h"
#include <QVariantList>
#include <sqlite3.h>

namespace Battery {
class Store {
public:
    Store();
    ~Store();
    bool open(const QString &path,bool writer);
    QString error() const { return failure; }
    bool append(const Sample &sample);
    bool heartbeat(const QString &run,int interval,bool paused,const QString &error);
    bool closeSession(const QString &reason);
    bool recordEvent(const Sample &sample,const QString &kind,const QString &text,int cooldownSeconds=0);
    QVariantList history(qint64 begin,qint64 end,QVariantMap *summary,qint64 sessionId=0,const QString &batteryKey=QString());
    QVariantList query(const QString &sql,const QVariantList &bindings=QVariantList());
    QVariantMap capacitySession(const QString &mode,int batteryId,qint64 sessionId=0,const QString &batteryKey=QString());
    QVariantMap capacitySummary(const QString &key,const QString &activeKey,const QVariantMap &systemSample=QVariantMap());
    bool setBattery(const QString &key,const QString &label);
    bool registerBattery(const QString &key,const QString &label);
    bool startTest(const QString &id,const QString &key,const QString &label,int seconds,const Sample &first);
    bool finishTest(const QString &reason,bool completed,const QString &request=QString(),const Sample *last=0);
    QString runningTest() const { return testId; }
    qint64 testDeadline() const;
    bool execute(const QString &sql,const QVariantList &bindings=QVariantList());
    sqlite3 *handle() const { return db; }
private:
    sqlite3 *db;
    QString failure;
    QString databasePath;
    Sample previous;
    bool havePrevious;
    qint64 session;
    int count,startSoc,lastSoc;
    bool stableSoc;
    double mah,mwh,covered,energyCovered;
    qint64 startMono;
    QString pendingBreak;
    QString batteryKey;
    QString testId;
    bool appendTest(const Sample &sample,const Sample &prior,bool havePrior,const Step &step,qint64 sampleId);
    bool beginSession(const Sample &s);
};
}
#endif
