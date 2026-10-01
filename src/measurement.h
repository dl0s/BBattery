#ifndef BBATTERY_MEASUREMENT_H
#define BBATTERY_MEASUREMENT_H
#include <QVariantMap>
#include <QVariantList>
#include <QString>
#include <QStringList>
#include <QtGlobal>

namespace Battery {
struct Sample {
    qint64 utc, mono;
    QString run, mode, charger, phase, raw, error;
    int batteryId, interval;
    bool ready;
    QVariantMap values;
    Sample();
    bool has(const char *name) const;
    double value(const char *name) const;
};
struct Step {
    QString breakReason;
    double seconds, charge, energy;
    bool chargeValid, energyValid;
    Step();
};
qint64 utcMillis();
qint64 monoMillis();
QVariant number(double value, double low, double high);
QString modeFor(const Sample &sample);
Step integrate(const Sample &previous, const Sample &current);
QVariant capacityEstimate(const QString &mode, double charge, double coverage,
                          int startSoc, int endSoc, bool closed, bool stableSoc);
QString modeLabel(const QString &mode);
bool selfTest(QString *report);
bool connected(const QString &charger);
QVariantMap sourceDetails(const QString &raw);
class HistorySummary {
public:
    HistorySummary(qint64 begin,qint64 end);
    void add(const QVariantMap &row);
    QVariantMap result()const;
private:
    qint64 begin,end;
    QVariantMap previous;
    int count,gaps;
    double covered,charge,discharge,chargeEnergy,dischargeEnergy,energySeconds,integrationSeconds;
    double sums[3],seconds[3],minimums[3],maximums[3];
    int valid[3];
};
struct AlertConfig {
    bool enabled;
    int lowSoc,highSoc,temperature;
    AlertConfig():enabled(true),lowSoc(20),highSoc(90),temperature(45){}
};
class AlertMonitor {
public:
    AlertMonitor();
    QStringList evaluate(const Sample &sample,const AlertConfig &config);
private:
    bool low,high,hot;
};
}
#endif
