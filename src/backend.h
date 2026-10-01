#ifndef BBATTERY_BACKEND_H
#define BBATTERY_BACKEND_H
#include "store.h"
#include <bb/cascades/ArrayDataModel>
#include <bb/cascades/Image>
#include <QTimer>
#include <QDateTime>

class HistoryLoader;
class InspectionLoader;
class Backend:public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantMap live READ live NOTIFY changed)
    Q_PROPERTY(QVariantMap quality READ quality NOTIFY changed)
    Q_PROPERTY(QVariantMap detail READ detail NOTIFY detailChanged)
    Q_PROPERTY(QVariantMap history READ history NOTIFY chartsChanged)
    Q_PROPERTY(QVariantMap alerts READ alerts NOTIFY changed)
    Q_PROPERTY(QVariantMap capacity READ capacity NOTIFY changed)
    Q_PROPERTY(bb::cascades::DataModel* sessions READ sessions CONSTANT)
    Q_PROPERTY(bb::cascades::DataModel* events READ events CONSTANT)
    Q_PROPERTY(int sessionCount READ sessionCount NOTIFY changed)
    Q_PROPERTY(int eventCount READ eventCount NOTIFY changed)
    Q_PROPERTY(bool following READ following NOTIFY changed)
    Q_PROPERTY(QDateTime historyDate READ historyDate NOTIFY changed)
    Q_PROPERTY(QVariant overviewImage READ overviewImage NOTIFY chartsChanged)
    Q_PROPERTY(QVariant socImage READ socImage NOTIFY chartsChanged)
    Q_PROPERTY(QVariant parameterImage READ parameterImage NOTIFY chartsChanged)
    Q_PROPERTY(QVariant detailSocImage READ detailSocImage NOTIFY detailChanged)
    Q_PROPERTY(QVariant detailCurrentImage READ detailCurrentImage NOTIFY detailChanged)
    Q_PROPERTY(QVariantMap overviewAxes READ overviewAxes NOTIFY chartsChanged)
    Q_PROPERTY(QVariantMap socAxes READ socAxes NOTIFY chartsChanged)
    Q_PROPERTY(QVariantMap parameterAxes READ parameterAxes NOTIFY chartsChanged)
    Q_PROPERTY(QVariantMap detailSocAxes READ detailSocAxes NOTIFY detailChanged)
    Q_PROPERTY(QVariantMap detailCurrentAxes READ detailCurrentAxes NOTIFY detailChanged)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString inspection READ inspection NOTIFY inspectionChanged)
    Q_PROPERTY(QVariantMap inspectionCursor READ inspectionCursor NOTIFY inspectionChanged)
    Q_PROPERTY(int windowHours READ windowHours NOTIFY changed)
    Q_PROPERTY(int interval READ interval NOTIFY changed)
    Q_PROPERTY(bool paused READ paused NOTIFY changed)
    Q_PROPERTY(QString parameter READ parameter NOTIFY changed)
public:
    Backend();
    ~Backend();
    QVariantMap live()const{return current;}
    QVariantMap quality()const{return assessment;}
    QVariantMap detail()const{return selected;}
    QVariantMap history()const{return historyStats;}
    QVariantMap alerts()const{return alertSettings;}
    QVariantMap capacity()const{return capacityValues;}
    bb::cascades::DataModel *sessions()const{return records;}
    bb::cascades::DataModel *events()const{return eventRecords;}
    int sessionCount()const{return records->size();}
    int eventCount()const{return eventRecords->size();}
    bool following()const{return endTime==0;}
    QDateTime historyDate()const{return QDateTime::fromMSecsSinceEpoch(endTime?endTime:Battery::utcMillis());}
    QVariant overviewImage()const{return QVariant::fromValue(overview);}
    QVariant socImage()const{return QVariant::fromValue(soc);}
    QVariant parameterImage()const{return QVariant::fromValue(other);}
    QVariant detailSocImage()const{return QVariant::fromValue(detailSoc);}
    QVariant detailCurrentImage()const{return QVariant::fromValue(detailCurrent);}
    QVariantMap overviewAxes()const{return overviewLabels;}
    QVariantMap socAxes()const{return socLabels;}
    QVariantMap parameterAxes()const{return otherLabels;}
    QVariantMap detailSocAxes()const{return detailSocLabels;}
    QVariantMap detailCurrentAxes()const{return detailCurrentLabels;}
    QString status()const{return message;}
    QString inspection()const{return pointText;}
    QVariantMap inspectionCursor()const;
    int windowHours()const{return hours;}
    int interval()const{return sampleInterval;}
    bool paused()const{return isPaused;}
    QString parameter()const{return channel;}
    void setScene(QObject *s){scene=s;}
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void setWindow(int value);
    Q_INVOKABLE void setParameter(const QString &value);
    Q_INVOKABLE void moveWindow(int direction);
    Q_INVOKABLE void browseDate(const QDateTime &date);
    Q_INVOKABLE void followLatest();
    Q_INVOKABLE void clearInspection();
    Q_INVOKABLE void configureAlerts(bool enabled,int low,int high,int temperature);
    Q_INVOKABLE void filterSessions(const QString &mode);
    Q_INVOKABLE void selectSession(qint64 id);
    Q_INVOKABLE void inspect(double x,double width,bool immediate=false);
    Q_INVOKABLE void configure(int interval,bool paused);
    Q_INVOKABLE void exportData(bool selectedSession=false);
signals:
    void changed();
    void chartsChanged();
    void detailChanged();
    void inspectionChanged();
    void exported(const QString &message);
    void alerted(const QString &message);
private slots:
    void diagnostic();
    void historyReady();
    void processInspection();
    void inspectionReady();
private:
    Battery::Store db;
    bb::cascades::ArrayDataModel *records,*eventRecords;
    QVariantMap current,assessment,selected,historyStats,alertSettings;
    QVariantMap capacityValues;
    qint64 capacitySessionId;
    int capacityBatteryId;
    QVariantMap overviewLabels,socLabels,otherLabels,detailSocLabels,detailCurrentLabels;
    QVariantList points,detailPoints,markers;
    bb::cascades::Image overview,soc,other,detailSoc,detailCurrent;
    QTimer refreshTimer,diagnosticTimer,inspectionTimer;
    QString directory,message,pointText,channel,sessionFilter;
    int hours,sampleInterval;
    bool isPaused;
    qint64 sampleId,detailId;
    qint64 endTime,rangeBegin,rangeEnd,inspectionTime,lastEventId;
    QObject *scene;
    HistoryLoader *historyLoader;
    InspectionLoader *inspectionLoader;
    int historyGeneration,loadedGeneration;
    int chartRenderCount;
    qint64 nextHistoryRefresh;
    bool historyPending,historyBusy;
    qint64 requestedInspectionTime,cachedInspectionTarget,cachedInspectionBegin,cachedInspectionEnd;
    qint64 inspectionBenchmarkStarted;
    int inspectionGeneration,inspectionLookupCount,inspectionBenchmarkLookups;
    bool inspectionPending,inspectionBusy,inspectionCacheValid;
    QVariantList inspectionSamples;
    QVariantMap inspectionResponse;
    void updateCharts();
    void updateRecords();
    QVariantMap formatSession(const QVariantMap &row,bool capacityVerified=false)const;
    void updateCapacity(const QVariantMap &sample);
    bb::cascades::Image chart(const QVariantList &rows,const QString &field,int height=228,
        const QString &save=QString(),QVariantMap *labels=0,qint64 begin=0,qint64 end=0,
        const QVariantList &events=QVariantList());
    void formatHistory();
    void resolveInspection(qint64 target,const QVariantList &nearby,const QString &error=QString());
    void completeInspectionResponse();
    void writeDiagnosticResponse(const QVariantMap &response);
    bool saveSettings(int interval,bool paused);
};
#endif
