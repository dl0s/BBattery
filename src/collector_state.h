#ifndef BBATTERY_COLLECTOR_STATE_H
#define BBATTERY_COLLECTOR_STATE_H
#include "measurement.h"
#include <QFile>
#include <QTextStream>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <signal.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>

namespace Battery {
// The process lock is never removed, including after a crash or reboot.
enum CollectorHealth { CollectorReady=0, CollectorStopped=10, CollectorStarting=11, CollectorUnknown=78 };
inline int openCollectorLock(const QString &directory,bool create){
    QByteArray path=QFile::encodeName(directory+"/collector.lock");struct stat before,opened;
    bool exists=lstat(path.constData(),&before)==0;
    if(!exists&&errno!=ENOENT)return -1;
    if(exists&&(!S_ISREG(before.st_mode)||before.st_nlink!=1)){errno=EINVAL;return -1;}
    int fd=::open(path.constData(),O_RDWR|(!exists&&create?O_CREAT|O_EXCL:0),0600);
    if(fd<0)return -1;
    if(fstat(fd,&opened)!=0||!S_ISREG(opened.st_mode)||opened.st_nlink!=1
       ||(exists&&(opened.st_ino!=before.st_ino||opened.st_dev!=before.st_dev))){::close(fd);errno=EINVAL;return -1;}
    return fd;
}
inline CollectorHealth collectorHealth(const QString &directory,const QVariantMap &state){
    int fd=openCollectorLock(directory,false);
    if(fd<0)return errno==ENOENT?CollectorStopped:CollectorUnknown;
    int rc=flock(fd,LOCK_EX|LOCK_NB),error=errno;::close(fd);
    if(rc==0)return CollectorStopped;
    if(error!=EAGAIN&&error!=EWOULDBLOCK)return CollectorUnknown;
    struct stat identityInfo;
    if(lstat(QFile::encodeName(directory+"/collector.instance").constData(),&identityInfo)!=0)return CollectorStarting;
    if(!S_ISREG(identityInfo.st_mode)||identityInfo.st_nlink!=1)return CollectorUnknown;
    QFile identity(directory+"/collector.instance");
    if(!identity.open(QIODevice::ReadOnly)||identity.size()>256)return CollectorStarting;
    const QStringList fields=QString::fromUtf8(identity.readAll()).trimmed().split(' ');
    if(fields.size()!=2)return CollectorUnknown;
    bool valid=false;const qint64 pid=fields[0].toLongLong(&valid);
    if(!valid||pid<=0||pid>2147483647||::kill(pid_t(pid),0)!=0)return CollectorUnknown;
    const qint64 age=monoMillis()-state.value("mono_ms").toLongLong();
    if(state.isEmpty()||state.value("pid").toLongLong()!=pid||state.value("run_id").toString()!=fields[1]
       ||state.value("euid").toLongLong()!=qint64(geteuid())||age<0||age>=45000
       ||!state.value("error").toString().isEmpty())return CollectorStarting;
    return CollectorReady;
}
inline bool writeCollectorInstance(const QString &directory,const QString &run){
    const QString temporary=directory+QString("/collector.instance.%1.next").arg(getpid());
    QFile file(temporary);
    if(!file.open(QIODevice::WriteOnly|QIODevice::Truncate))return false;
    QByteArray body=QByteArray::number(getpid())+" "+run.toUtf8()+"\n";
    bool ok=file.write(body)==body.size()&&file.flush()&&::fsync(file.handle())==0;
    file.close();
    if(ok)ok=::rename(QFile::encodeName(temporary).constData(),QFile::encodeName(directory+"/collector.instance").constData())==0;
    if(!ok)QFile::remove(temporary);
    return ok;
}
}
#endif
