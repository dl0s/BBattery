#ifndef BBATTERY_BACKEND_H
#define BBATTERY_BACKEND_H
#include "store.h"
#include <bb/cascades/ArrayDataModel>
#include <QTimer>

class TestReader;
class ExportWorker;
class Backend:public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantMap test READ test NOTIFY changed)
    Q_PROPERTY(QVariantMap live READ live NOTIFY changed)
    Q_PROPERTY(QVariantMap detail READ detail NOTIFY changed)
    Q_PROPERTY(QVariantMap battery READ battery NOTIFY batteriesChanged)
    Q_PROPERTY(QVariantList batteries READ batteries NOTIFY batteriesChanged)
    Q_PROPERTY(bb::cascades::DataModel* results READ results CONSTANT)
    Q_PROPERTY(int resultCount READ resultCount NOTIFY changed)
    Q_PROPERTY(bool running READ running NOTIFY changed)
    Q_PROPERTY(bool pending READ pending NOTIFY changed)
    Q_PROPERTY(bool ready READ ready NOTIFY changed)
    Q_PROPERTY(bool hasMore READ hasMore NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
public:
    Backend();
    ~Backend();
    QVariantMap test()const{return currentTest;}
    QVariantMap live()const{return current;}
    QVariantMap detail()const{return selected;}
    QVariantMap battery()const{return batteryState;}
    QVariantList batteries()const{return batteryItems;}
    bb::cascades::DataModel *results()const{return records;}
    int resultCount()const{return records->size();}
    bool running()const{return currentTest.value("status").toString()=="running";}
    bool pending()const{return !pendingRequest.isEmpty();}
    bool ready()const{return collectorReady;}
    bool hasMore()const{return more;}
    QString status()const{return message;}
    void setScene(QObject *value){scene=value;}
    Q_INVOKABLE void refresh();
    Q_INVOKABLE bool startTest(int minutes,int interval=30);
    Q_INVOKABLE bool stopTest();
    Q_INVOKABLE QString createBattery(const QString &label);
    Q_INVOKABLE bool renameBattery(const QString &key,const QString &label);
    Q_INVOKABLE bool useBattery(const QString &key);
    Q_INVOKABLE void viewBattery(const QString &key);
    Q_INVOKABLE void selectTest(const QString &id);
    Q_INVOKABLE void loadMore();
    Q_INVOKABLE void exportData(bool allHistory=false);
signals:
    void changed();
    void batteriesChanged();
    void notified(const QString &message);
private slots:
    void readReady();
    void exportReady();
    void diagnostic();
private:
    Battery::Store db;
    bb::cascades::ArrayDataModel *records;
    TestReader *reader;
    ExportWorker *exporter;
    QTimer refreshTimer,diagnosticTimer;
    QString directory,activeBattery,viewedBattery,selectedId,pendingRequest,message;
    QVariantMap currentTest,current,selected,batteryState,collectorState;
    QVariantList batteryItems,recordRows;
    QObject *scene;
    int limit;
    bool busy,again,collectorReady,more;
    qint64 pendingExpires;
    void loadBatteries();
    QVariantMap formatTest(const QVariantMap &row)const;
    bool request(const QString &operation,int seconds=0,int interval=30);
    void writeResponse(const QVariantMap &response);
};
#endif
