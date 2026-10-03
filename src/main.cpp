#include "backend.h"
#include <bb/cascades/Application>
#include <bb/cascades/QmlDocument>
#include <bb/cascades/TabbedPane>
#include <bb/cascades/Page>
#include <bb/cascades/Container>
#include <bb/cascades/Label>
#include <QtDeclarative/QDeclarativeError>
#include <QTextCodec>
#include <cstdio>

int main(int argc,char **argv){
    const qint64 startup=Battery::monoMillis();
    std::fputs("BBattery 0.1.0.8 startup\n",stderr);std::fflush(stderr);
    bb::cascades::Application app(argc,argv);
    std::fputs("BBattery application: ready\n",stderr);std::fflush(stderr);
    QTextCodec::setCodecForCStrings(QTextCodec::codecForName("UTF-8"));
    Backend backend;
    bb::cascades::QmlDocument *document=bb::cascades::QmlDocument::create("asset:///main.qml").parent(&app);
    document->setContextProperty("backend",&backend);
    bb::cascades::TabbedPane *pane=document->createRootObject<bb::cascades::TabbedPane>();
    if(!pane){
        foreach(const QDeclarativeError &e,document->errors())std::fprintf(stderr,"QML: %s\n",e.toString().toUtf8().constData());
        bb::cascades::Page *page=new bb::cascades::Page;bb::cascades::Container *container=new bb::cascades::Container;
        bb::cascades::Label *label=new bb::cascades::Label;label->setText(QString::fromUtf8("BBattery 界面加载失败"));container->add(label);page->setContent(container);
        app.setScene(page);
    }else{
        backend.setScene(pane);app.setScene(pane);
        std::fputs("BBattery 0.1.0.8 native scene ready\n",stderr);
        std::fprintf(stderr,"BBattery scene prepared: %lld ms\n",static_cast<long long>(Battery::monoMillis()-startup));std::fflush(stderr);
    }
    return app.exec();
}
