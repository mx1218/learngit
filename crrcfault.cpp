#include "crrcfault.h"
#include "crrcmvb.h"
#include <QFile>
#include <QStringList>
#include <QDateTime>
#include <stdlib.h>
#include "qdebug.h"
#include "QMutexLocker"

#define _CRRC_FAULT_VERSION 3.2

#define _CRRC_MAX_HISTORY_FAULT_RECORD 5000
#define _CRRC_PORT_SIZE 32
#define _CRRC_QUERY_FAULT_LEVEL 4


CrrcFault* CrrcFault::crrcFault = NULL;

CrrcFault* CrrcFault::getCrrcFault()
{
    return crrcFault;
}

bool CrrcFault::initCrrcFault(QString faultListPath, QString historyFilePath)
{

    if(crrcFault == NULL)
    {
        crrcFault = new CrrcFault(faultListPath, historyFilePath);
    }

    if(crrcFault == NULL)
    {
        return false;
    }else
    {
        return true;
    }
}

CrrcFault::CrrcFault(QString databaseFaultTypeName, QString databaseHistoryFaultName)
    : databaseFaultTypeName(databaseFaultTypeName), databaseHistoryFaultName(databaseHistoryFaultName)
{
    crrcFaultMapper = new CrrcFaultMapper(databaseFaultTypeName,databaseHistoryFaultName);

    if(crrcFaultMapper->openDatabase())
    {
        this->readFaultTypeListFile();
        this->readHistoryFaultFile();
    }else
    {
        qDebug()<<"DB open fail";
    }
    unconfirmFault123count = 0;
}
void CrrcFault::closeDB()
{
    //close thread
    this->terminate();
    this->crrcFaultMapper->closeDatabase();
}

void CrrcFault::getLocalDateTime(QDateTime datetime)
{
    this->m_Localdatetime = datetime;
}


void CrrcFault::run()
{
    while(1)
    {
        // fault scanning 200ms per period.

        if (NULL == CrrcMvb::getCrrcMvb())
        {
            qDebug() << "there is no crrcMvb class, please check it";

            return;
        }
        bool tmp_NewFaultOccur = false;
        int t_circlefaultcnt = 0;
        {
            QMutexLocker locker(&m_lock);
           if(!this->crrcFaultMapper->GetNextHistoryFaultID(t_circlefaultcnt))
            {
                return;
            };// record max History_ID since step into circleqde
        }

        //QTime timeStart(QTime::currentTime());


        foreach (quint32 key, this->FaultTypeHash.keys())
        {
            QMutexLocker locker(&m_lock);
            if(!CrrcMvb::getCrrcMvb()->getportexist(FaultTypeHash[key].PortAddress))
            {
                //not defined ports ;
            }else
            {
                if (FaultTypeHash[key].FaultValid == CrrcMvb::getCrrcMvb()->getBool(FaultTypeHash[key].PortAddress, FaultTypeHash[key].ByteoffAddress, FaultTypeHash[key].BitoffAddress))
                {
                   if (this->CurrentFaultHash.contains(key) == false)
                   {
                       FaultBean t_faultBean;
                       t_faultBean.HistoryID = t_circlefaultcnt;
                       t_faultBean.StartTime = m_Localdatetime.toString("yyyy-MM-dd hh:mm:ss");
                       t_faultBean.ID = key;
                       if(_CRRC_QUERY_FAULT_LEVEL < FaultTypeHash[key].FaultLevel)
                           t_faultBean.IsConfirm = true;
                       else
                       t_faultBean.IsConfirm = false;
                       //create insert faultbean hash
                       this->InsertHistoryFaultHash.insert(t_circlefaultcnt,t_faultBean);
                       t_circlefaultcnt++;
                       //qDebug()<<key <<t_circlefaultcnt;

                       this->NewFaultDetect = true;
                       tmp_NewFaultOccur = true;
                       this->NewFaultOccur  =true;
                   }

                }
                else if (this->CurrentFaultHash.contains(key) == true)
                {
                    FaultBean t_faultBean;
                    t_faultBean.EndTime = m_Localdatetime.toString("yyyy-MM-dd hh:mm:ss");

                    //insert update historyfault bean hash
                    this->UpdateHistoryFaultHash.insert(this->CurrentFaultHash.value(key).HistoryID,t_faultBean);

                    tmp_NewFaultOccur = true;
                    this->NewFaultOccur  =true;

                }
            }
        }


        if(tmp_NewFaultOccur)
        {
            QMutexLocker locker(&m_lock);
            //when new fault occurs, refresh history and current fault list,then load them from DB and RAM
            this->currentFaultList.clear();
            this->historyFaultList.clear();
            this->unconfirmFaultList.clear();
            this->CurrentFaultHash.clear();

            {
                if(this->crrcFaultMapper->InsertHistoryFault(InsertHistoryFaultHash)&&
                this->crrcFaultMapper->UpdateHistoryFault(UpdateHistoryFaultHash))
                {
                    if(this->crrcFaultMapper->GetHistoryFault(this->historyFaultList)&&
                            this->crrcFaultMapper->GetCurrentFault(this->currentFaultList,this->CurrentFaultHash))
                    {

                    }else
                    {
                        qDebug() << "get fault err ";
                        this->InsertHistoryFaultHash.clear();
                        this->UpdateHistoryFaultHash.clear();
                        return;
                    }
                    this->crrcFaultMapper->GetUnconfirmFault(this->unconfirmFaultList);

                    // uncomfirm fault query, remove level >_CRRC_QUERY_FAULT_LEVEL fault
                    for(int i = 0; i < unconfirmFaultList.size();i++)
                    {
                        if(FaultTypeHash[unconfirmFaultList.at(i).ID].FaultLevel > _CRRC_QUERY_FAULT_LEVEL)
                        {
                            unconfirmFaultList.removeAt(i);
                        }
                    }

                }
                this->InsertHistoryFaultHash.clear();
                this->UpdateHistoryFaultHash.clear();
            }
            tmp_NewFaultOccur = false;

        }
        //qDebug() << "scanning fault : " << timeStart.msecsTo(QTime::currentTime());
        msleep(200);

    }

}

void CrrcFault::deleteConfirmFault(unsigned short int index)
{
    QMutexLocker locker(&m_lock);

    if(this->unconfirmFaultList.size()>0 && index<this->unconfirmFaultList.size())
    {
        // update the max index element of unconfirm list,then reload unconfirm fault list
        this->crrcFaultMapper->UpdateUnconfirmFault(this->unconfirmFaultList.at(index).HistoryID);

        unconfirmFaultList.clear();
        this->crrcFaultMapper->GetUnconfirmFault(this->unconfirmFaultList);

        //update historyfault and current fault(get confirm information)
        this->historyFaultList.clear();
        this->crrcFaultMapper->GetHistoryFault(historyFaultList);
        this->currentFaultList.clear();
        this->CurrentFaultHash.clear();
        this->crrcFaultMapper->GetCurrentFault(this->currentFaultList,this->CurrentFaultHash);
        this->NewFaultOccur = true;

    }
}
// delete confirm fault by select current fault
void CrrcFault::deleteConfirmFaultByCurrentFaultList(unsigned short int index)
{
    QMutexLocker locker(&m_lock);

    if(this->currentFaultList.size()>0 && index<this->currentFaultList.size())
    {
        this->crrcFaultMapper->UpdateUnconfirmFault(this->currentFaultList.at(index).HistoryID);
        unconfirmFaultList.clear();
        this->crrcFaultMapper->GetUnconfirmFault(this->unconfirmFaultList);

        this->historyFaultList.clear();
        this->crrcFaultMapper->GetHistoryFault(historyFaultList);
        this->currentFaultList.clear();
        this->CurrentFaultHash.clear();
        this->crrcFaultMapper->GetCurrentFault(this->currentFaultList,this->CurrentFaultHash);
        this->NewFaultOccur = true;
    }
}

void CrrcFault::deleteAllconfirmFault()
{
    QMutexLocker locker(&m_lock);

    for(int i = unconfirmFaultList.size()-1; i >-1 ;i--)
    {
        this->crrcFaultMapper->UpdateUnconfirmFault(this->unconfirmFaultList.at(i).HistoryID);
    }
    unconfirmFaultList.clear();
    this->crrcFaultMapper->GetUnconfirmFault(this->unconfirmFaultList);
}
QString CrrcFault::getCurrentFaultDate(unsigned short int index)
{
    QMutexLocker locker(&m_lock);

    if (index >= this->currentFaultList.size())
    {
        qDebug() << "the index: "<<index<<" is larger than the current fault list"<<this->currentFaultList.size()<<", please check it" << __FILE__ << __LINE__;

        return QString("");
    }
    else
    {
        QDateTime dateTime = QDateTime::fromString(this->currentFaultList.at(index).StartTime, QString("yyyy-MM-dd hh:mm:ss"));

        return dateTime.toString("yyyy-MM-dd");
    }
}

QString CrrcFault::getCurrentFaultTime(unsigned short int index)
{
    QMutexLocker locker(&m_lock);

    if (index >= this->currentFaultList.size())
    {
        qDebug() << "the index: "<<index<<" is larger than the current fault list"<<this->currentFaultList.size()<<", please check it" << __FILE__ << __LINE__;

        return QString("");
    }
    else
    {
        QDateTime dateTime = QDateTime::fromString(this->currentFaultList.at(index).StartTime, QString("yyyy-MM-dd hh:mm:ss"));

        return dateTime.toString("hh:mm:ss");
    }
}

QString CrrcFault::getCurrentFaultCode(unsigned short index)
{
    QMutexLocker locker(&m_lock);

    if (index >= this->currentFaultList.size())
    {
        qDebug() << "the index: "<<index<<" is larger than the current fault list"<<this->currentFaultList.size()<<", please check it" << __FILE__ << __LINE__;

        return QString("");
    }
    else
    {
        return this->FaultTypeHash[this->currentFaultList.at(index).ID].FaultCode.trimmed();
    }
}

QString CrrcFault::getCurrentFaultName(unsigned short index)
{
    QMutexLocker locker(&m_lock);

    if (index >= this->currentFaultList.size())
    {
        qDebug() << "the index: "<<index<<" is larger than the current fault list"<<this->currentFaultList.size()<<", please check it" << __FILE__ << __LINE__;

        return QString("");
    }
    else
    {
        return this->FaultTypeHash[this->currentFaultList.at(index).ID].FaultName.trimmed();
    }
}

QString CrrcFault::getCurrentFaultDevice(unsigned short int index)
{
    QMutexLocker locker(&m_lock);

    if (index >= this->currentFaultList.size())
    {
        qDebug() << "the index: "<<index<<" is larger than the current fault list"<<this->currentFaultList.size()<<", please check it" << __FILE__ << __LINE__;

        return QString("");
    }
    else
    {
        return this->FaultTypeHash[this->currentFaultList.at(index).ID].FaultDevice.trimmed();
    }
}

QString CrrcFault::getCurrentFaultPosition(unsigned short int index)
{
    QMutexLocker locker(&m_lock);

    if (index >= this->currentFaultList.size())
    {
        qDebug() << "the index: "<<index<<" is larger than the current fault list"<<this->currentFaultList.size()<<", please check it" << __FILE__ << __LINE__;

        return QString("");
    }
    else
    {
        return this->FaultTypeHash[this->currentFaultList.at(index).ID].FaultPosition.trimmed();
    }
}

QString CrrcFault::getCurrentFaultDescription(unsigned short index)
{
    QMutexLocker locker(&m_lock);

    if (index >= this->currentFaultList.size())
    {
        qDebug() << "the index: "<<index<<" is larger than the current fault list"<<this->currentFaultList.size()<<", please check it" << __FILE__ << __LINE__;

        return QString("");
    }
    else
    {
        return this->FaultTypeHash[this->currentFaultList.at(index).ID].FaultDescription.trimmed();
    }
}
QString CrrcFault::getCurrentFaultLevel(unsigned short int index)
{
    QMutexLocker locker(&m_lock);

    if (index >= this->currentFaultList.size())
    {
        qDebug() << "the index: "<<index<<" is larger than the history fault list"<<this->currentFaultList.size()<<", please check it" << __FILE__ << __LINE__;

        return QString("");
    }
    else
    {
        return QString::number(this->FaultTypeHash[this->currentFaultList.at(index).ID].FaultLevel);
    }
}
bool CrrcFault::getCurrentFaultConfirm(unsigned short int index)
{
    QMutexLocker locker(&m_lock);

    if (index >= this->currentFaultList.size())
    {
        qDebug() << "the index: "<<index<<" is larger than the current fault list"<<this->currentFaultList.size()<<", please check it" << __FILE__ << __LINE__;

        return false;
    }
    else
    {
        return this->currentFaultList.at(index).IsConfirm;
    }
}

QString CrrcFault::getHistoryFaultLevel(unsigned short int index)
{
    QMutexLocker locker(&m_lock);

    if (index >= this->historyFaultList.size())
    {
        qDebug() << "the index: "<<index<<" is larger than the history fault list"<<this->historyFaultList.size()<<", please check it" << __FILE__ << __LINE__;

        return QString("");
    }
    else
    {
        return QString::number(this->FaultTypeHash[this->historyFaultList.at(index).ID].FaultLevel);
    }
}

QString CrrcFault::getHistoryFaultStartDate(unsigned short int index)
{
    QMutexLocker locker(&m_lock);

    if (index >= this->historyFaultList.size())
    {
        qDebug() << "the index: "<<index<<" is larger than the history fault list"<<this->historyFaultList.size()<<", please check it" << __FILE__ << __LINE__;

        return QString("");
    }
    else
    {
        QDateTime dateTime = QDateTime::fromString(this->historyFaultList.at(index).StartTime, QString("yyyy-MM-dd hh:mm:ss"));

        return dateTime.toString("yyyy-MM-dd");
    }
}

QString CrrcFault::getHistoryFaultStartTime(unsigned short int index)
{
    QMutexLocker locker(&m_lock);

    if (index >= this->historyFaultList.size())
    {
        qDebug() << "the index: "<<index<<" is larger than the history fault list"<<this->historyFaultList.size()<<", please check it" << __FILE__ << __LINE__;

        return QString("");
    }
    else
    {
        QDateTime dateTime = QDateTime::fromString(this->historyFaultList.at(index).StartTime, QString("yyyy-MM-dd hh:mm:ss"));

        return dateTime.toString("hh:mm:ss");
    }
}

QString CrrcFault::getHistoryFaultEndDate(unsigned short int index)
{
    QMutexLocker locker(&m_lock);

    if (index >= this->historyFaultList.size())
    {
        qDebug() << "the index: "<<index<<" is larger than the history fault list"<<this->historyFaultList.size()<<", please check it" << __FILE__ << __LINE__;

        return QString("");
    }
    else
    {
        QDateTime dateTime = QDateTime::fromString(this->historyFaultList.at(index).EndTime, QString("yyyy-MM-dd hh:mm:ss"));

        return dateTime.toString("yyyy-MM-dd");
    }
}

QString CrrcFault::getHistoryFaultEndTime(unsigned short int index)
{
    QMutexLocker locker(&m_lock);

    if (index >= this->historyFaultList.size())
    {
        qDebug() << "the index: "<<index<<" is larger than the history fault list"<<this->historyFaultList.size()<<", please check it" << __FILE__ << __LINE__;

        return QString("");
    }
    else
    {
        QDateTime dateTime = QDateTime::fromString(this->historyFaultList.at(index).EndTime, QString("yyyy-MM-dd hh:mm:ss"));

        return dateTime.toString("hh:mm:ss");
    }
}

QString CrrcFault::getHistoryFaultCode(unsigned short int index)
{
    QMutexLocker locker(&m_lock);

    if (index >= this->historyFaultList.size())
    {
        qDebug() << "the index: "<<index<<" is larger than the history fault list"<<this->historyFaultList.size()<<", please check it" << __FILE__ << __LINE__;

        return QString("");
    }
    else
    {
        return this->FaultTypeHash[this->historyFaultList.at(index).ID].FaultCode.trimmed();
    }
}

QString CrrcFault::getHistoryFaultName(unsigned short int index)
{
    QMutexLocker locker(&m_lock);

    if (index >= this->historyFaultList.size())
    {
        qDebug() << "the index: "<<index<<" is larger than the history fault list"<<this->historyFaultList.size()<<", please check it" << __FILE__ << __LINE__;

        return QString("");
    }
    else
    {
        return this->FaultTypeHash[this->historyFaultList.at(index).ID].FaultName.trimmed();
    }
}

QString CrrcFault::getHistoryFaultDevice(unsigned short int index)
{
    QMutexLocker locker(&m_lock);

    if (index >= this->historyFaultList.size())
    {
        qDebug() << "the index: "<<index<<" is larger than the history fault list"<<this->historyFaultList.size()<<", please check it" << __FILE__ << __LINE__;

        return QString("");
    }
    else
    {
        return this->FaultTypeHash[this->historyFaultList.at(index).ID].FaultDevice.trimmed();
    }
}

QString CrrcFault::getHistoryFaultPosition(unsigned short int index)
{
    QMutexLocker locker(&m_lock);

    if (index >= this->historyFaultList.size())
    {
        qDebug() << "the index: "<<index<<" is larger than the history fault list"<<this->historyFaultList.size()<<", please check it" << __FILE__ << __LINE__;

        return QString("");
    }
    else
    {
        return this->FaultTypeHash[this->historyFaultList.at(index).ID].FaultPosition.trimmed();
    }
}


QString CrrcFault::getHistoryFaultDescription(unsigned short int index)
{
    QMutexLocker locker(&m_lock);

    if (index >= this->historyFaultList.size())
    {
        qDebug() << "the index: "<<index<<" is larger than the history fault list"<<this->historyFaultList.size()<<", please check it" << __FILE__ << __LINE__;

        return QString("");
    }
    else
    {
        return this->FaultTypeHash[this->historyFaultList.at(index).ID].FaultDescription.trimmed();
    }
}
bool CrrcFault::getHistoryFaultConfirm(unsigned short int index)
{
    QMutexLocker locker(&m_lock);

    if (index >= this->historyFaultList.size())
    {
        qDebug() << "the index: "<<index<<" is larger than the history fault list"<<this->historyFaultList.size()<<", please check it" << __FILE__ << __LINE__;

        return false;
    }
    else
    {
        return this->historyFaultList.at(index).IsConfirm;
    }
}
void CrrcFault::readFaultTypeListFile()
{
    QMutexLocker locker(&m_lock);
    this->crrcFaultMapper->GetFaultTypeHash(this->FaultTypeHash);
}

void CrrcFault::readHistoryFaultFile()
{
    QMutexLocker locker(&m_lock);

    this->crrcFaultMapper->GetHistoryFault(historyFaultList);

    qDebug() << "there are" << this->historyFaultList.size() << "record in the history fault list file" << __FILE__ << __LINE__;

    this->crrcFaultMapper->GetCurrentFault(this->currentFaultList,this->CurrentFaultHash);
    this->crrcFaultMapper->GetUnconfirmFault(this->unconfirmFaultList);

    if(this->unconfirmFaultList.size()>0)
    {
        this->NewFaultOccur  = true;
    }
}




unsigned short int CrrcFault::getCurrentFaultListSize()
{
    QMutexLocker locker(&m_lock);
    return this->currentFaultList.size();
    //return 0;
}

unsigned short int CrrcFault::getHistoryFaultListSize()
{
    QMutexLocker locker(&m_lock);
    return this->historyFaultList.size();
    //return 0;
}

QString CrrcFault::getConfirmFaultLevel(unsigned short int index)
{
    QMutexLocker locker(&m_lock);

    if (index >= this->unconfirmFaultList.size())
    {
        qDebug() << "the index: "<<index<<" is larger than the Confirm fault list"<<this->unconfirmFaultList.size()<<", please check it" << __FILE__ << __LINE__;

        return QString("");
    }
    else
    {
        return QString::number(this->FaultTypeHash[this->unconfirmFaultList.at(index).ID].FaultLevel);
    }
}

QString CrrcFault::getConfirmFaultDate(unsigned short int index)
{
    QMutexLocker locker(&m_lock);

    if (index >= this->unconfirmFaultList.size())
    {
        qDebug() << "the index is larger than the Confirm fault list" << __FILE__ << __LINE__;

        return QString("");
    }
    else
    {
        QDateTime dateTime = QDateTime::fromString(this->unconfirmFaultList.at(index).StartTime, QString("yyyy-MM-dd hh:mm:ss"));

        return dateTime.toString("yyyy-MM-dd");
    }
}

QString CrrcFault::getConfirmFaultTime(unsigned short int index)
{
    QMutexLocker locker(&m_lock);

    if (index >= this->unconfirmFaultList.size())
    {
        qDebug() << "the index is larger than the Confirm fault list" << __FILE__ << __LINE__;

        return QString("");
    }
    else
    {
        QDateTime dateTime = QDateTime::fromString(this->unconfirmFaultList.at(index).StartTime, QString("yyyy-MM-dd hh:mm:ss"));

        return dateTime.toString("hh:mm:ss");
    }
}

QString CrrcFault::getConfirmFaultCode(unsigned short index)
{
    QMutexLocker locker(&m_lock);

    if (index >= this->unconfirmFaultList.size())
    {
        qDebug() << "the index is larger than the Confirm fault list, please check it" << __FILE__ << __LINE__;

        return QString("");
    }
    else
    {
        return this->FaultTypeHash[this->unconfirmFaultList.at(index).ID].FaultCode.trimmed();
    }
}

QString CrrcFault::getConfirmFaultName(unsigned short index)
{
    QMutexLocker locker(&m_lock);

    if (index >= this->unconfirmFaultList.size())
    {
        qDebug() << "the index is larger than the Confirm fault list, please check it" << __FILE__ << __LINE__;

        return QString("");
    }
    else
    {
        return this->FaultTypeHash[this->unconfirmFaultList.at(index).ID].FaultName.trimmed();
    }
}

QString CrrcFault::getConfirmFaultDevice(unsigned short int index)
{
    QMutexLocker locker(&m_lock);

    if (index >= this->unconfirmFaultList.size())
    {
        qDebug() << "the index is larger than the Confirm fault list, please check it" << __FILE__ << __LINE__;

        return QString("");
    }
    else
    {
        return this->FaultTypeHash[this->unconfirmFaultList.at(index).ID].FaultDevice.trimmed();
    }
}

QString CrrcFault::getConfirmFaultPosition(unsigned short int index)
{
    QMutexLocker locker(&m_lock);

    if (index >= this->unconfirmFaultList.size())
    {
        qDebug() << "the index is larger than the Confirm fault list, please check it" << __FILE__ << __LINE__;

        return QString("");
    }
    else
    {
        return this->FaultTypeHash[this->unconfirmFaultList.at(index).ID].FaultPosition.trimmed();
    }
}

QString CrrcFault::getConfirmFaultDescription(unsigned short index)
{
    QMutexLocker locker(&m_lock);

    if (index >= this->unconfirmFaultList.size())
    {
        qDebug() << "the index is larger than the Confirm fault list, please check it" << __FILE__ << __LINE__;

        return QString("");
    }
    else
    {
        return this->FaultTypeHash[this->unconfirmFaultList.at(index).ID].FaultDescription.trimmed();
    }
}
unsigned short int CrrcFault::getConfirmFaultListSize()
{
    QMutexLocker locker(&m_lock);

    return this->unconfirmFaultList.size();
}

unsigned short int CrrcFault::getConfirmFault123ListSize()
{
    QMutexLocker locker(&m_lock);

    unconfirmFault123count = this->unconfirmFaultList.size();

    if(unconfirmFault123count>0)
    {
        for(int i = 0; i < unconfirmFaultList.size();i++)
        {
            if((4 == FaultTypeHash[unconfirmFaultList.at(i).ID].FaultLevel) || (3 == FaultTypeHash[unconfirmFaultList.at(i).ID].FaultLevel))
            {
                unconfirmFault123count--;
            }
        }
    }else
    {
        unconfirmFault123count = 0;
    }
    return unconfirmFault123count;
}

unsigned int CrrcFault::getFaultCntOfEachVehicle(QString str)
{
    QMutexLocker locker(&m_lock);
    int res = 0;
    int t_size = this->historyFaultList.size();
    for(int i = 0;i<t_size;i++)
    {
        if(FaultTypeHash[this->historyFaultList[i].ID].FaultPosition.trimmed() == str)
            res++;
    }
    return res;

}
unsigned int CrrcFault::getFaultCntOfEachSystem(QString str)
{
    QMutexLocker locker(&m_lock);
    int res = 0;
    int t_size = this->historyFaultList.size();
    for(int i = 0;i<t_size;i++)
    {
        if(FaultTypeHash[this->historyFaultList[i].ID].FaultDevice.trimmed() == str)
            res++;
    }
    return res;

}
void CrrcFault::getQueryFaultOfEachSystem(QString str)
{
    QMutexLocker locker(&m_lock);
    QueryList.clear();
    int t_size = this->historyFaultList.size();
    for(int i = 0;i<t_size;i++)
    {
        //将historyfaultlist中的HID的下脚标传给筛选显示页面，筛选显示页面通过list中脚标位置，get出faulttype信息。
        if(FaultTypeHash[this->historyFaultList[i].ID].FaultDevice.trimmed() == str)
            QueryList.push_back(i);
    }
}
void CrrcFault::getQueryFaultOfEachVehicle(QString str)
{
    QMutexLocker locker(&m_lock);
    QueryList.clear();
    int t_size = this->historyFaultList.size();
    for(int i = 0;i<t_size;i++)
    {
        if(FaultTypeHash[this->historyFaultList[i].ID].FaultPosition.trimmed() == str)
            QueryList.push_back(i);

    }
}
bool CrrcFault::getQueryFaultOfEachCode(QString str)
{
    QMutexLocker locker(&m_lock);
    QueryList.clear();
    int t_size = this->historyFaultList.size();
    for(int i = 0;i<t_size;i++)
    {
        if(FaultTypeHash[this->historyFaultList[i].ID].FaultCode.trimmed() == str)
            QueryList.push_back(i);
    }
    if(QueryList.size() == 0)
        return false;
    else
        return true;
}

void CrrcFault::getCurrentQueryFaultOfLevel12()
{
    QMutexLocker locker(&m_lock);
    QueryList.clear();
    int t_size = this->currentFaultList.size();
    for (int i = 0; i < t_size; i++)
    {
        if(!FaultTypeHash.contains(this->currentFaultList[i].ID))
        {
            continue;
        }

        if ((FaultTypeHash[this->currentFaultList[i].ID].FaultLevel == 1)
            || (FaultTypeHash[this->currentFaultList[i].ID].FaultLevel == 2))
        {
            QueryList.push_back(i);
        }
    }
}

void CrrcFault::getCurrentQueryFaultOfLevel3()
{
    QMutexLocker locker(&m_lock);
    QueryList.clear();
    int t_size = this->currentFaultList.size();
    for (int i = 0; i < t_size; i++)
    {
        if(!FaultTypeHash.contains(this->currentFaultList[i].ID))
        {
            continue;
        }

        if (FaultTypeHash[this->currentFaultList[i].ID].FaultLevel == 3)
        {
            QueryList.push_back(i);
        }
    }
}

void CrrcFault::getCurrentQueryFaultOfLevel4()
{
    QMutexLocker locker(&m_lock);
    QueryList.clear();
    int t_size = this->currentFaultList.size();
    for (int i = 0; i < t_size; i++)
    {
        if(!FaultTypeHash.contains(this->currentFaultList[i].ID))
        {
            continue;
        }

        if (FaultTypeHash[this->currentFaultList[i].ID].FaultLevel == 4)
        {
            QueryList.push_back(i);
        }
    }
}

void CrrcFault::getHistoryQueryFaultOfLevel12()
{
    QMutexLocker locker(&m_lock);
    QueryList.clear();
    int t_size = this->historyFaultList.size();
    for (int i = 0; i < t_size; i++)
    {
        if(!FaultTypeHash.contains(this->historyFaultList[i].ID))
        {
            continue;
        }

        if (FaultTypeHash[this->historyFaultList[i].ID].FaultLevel == 1
                || FaultTypeHash[this->historyFaultList[i].ID].FaultLevel == 2)
        {
            QueryList.push_back(i);
        }

    }
}

void CrrcFault::getHistoryQueryFaultOfLevel3()
{
    QMutexLocker locker(&m_lock);
    QueryList.clear();
    int t_size = this->historyFaultList.size();
    for (int i = 0; i < t_size; i++)
    {
        if(!FaultTypeHash.contains(this->historyFaultList[i].ID))
        {
            continue;
        }

        if (FaultTypeHash[this->historyFaultList[i].ID].FaultLevel == 3)
        {
            QueryList.push_back(i);
        }
    }
}

void CrrcFault::getHistoryQueryFaultOfLevel4()
{
    QMutexLocker locker(&m_lock);
    QueryList.clear();
    int t_size = this->historyFaultList.size();
    for (int i = 0; i < t_size; i++)
    {
        if(!FaultTypeHash.contains(this->historyFaultList[i].ID))
        {
            continue;
        }

        if (FaultTypeHash[this->historyFaultList[i].ID].FaultLevel == 4)
        {
            QueryList.push_back(i);
        }
    }
}

QList<unsigned int> CrrcFault::getQueryFault()
{
    QMutexLocker locker(&m_lock);
    return this->QueryList;
}

QString CrrcFault::getFaultTypeVersion()
{
    QMutexLocker locker(&m_lock);
    return this->crrcFaultMapper->GetTypeDBInfo();
}
//根据系统和等级去查询故障，用于系统综合状态
bool CrrcFault::queryCurrentFaultCnt(QString system,int level,QString pos)
{
    QMutexLocker locker(&m_lock);
    QueryList.clear();
    int t_size = this->currentFaultList.size();

    // todo system and pos ALL

    if(pos == "ALL")
    {
        for(int i = 0;i<t_size;i++)
        {
            if(FaultTypeHash[this->currentFaultList[i].ID].FaultDevice.trimmed() == system &&
               FaultTypeHash[this->currentFaultList[i].ID].FaultLevel <= level)
                return true;
        }
    }else if(system == "ALL")
    {
        for(int i = 0;i<t_size;i++)
        {
            if(FaultTypeHash[this->currentFaultList[i].ID].FaultPosition.trimmed() == pos &&
               FaultTypeHash[this->currentFaultList[i].ID].FaultLevel <= level)
                return true;
        }
    }else
    {
        for(int i = 0;i<t_size;i++)
        {
            if(FaultTypeHash[this->currentFaultList[i].ID].FaultPosition.trimmed() == pos &&
               FaultTypeHash[this->currentFaultList[i].ID].FaultLevel <= level&&
               FaultTypeHash[this->currentFaultList[i].ID].FaultDevice.trimmed() == system )
                return true;
        }
    }
    return false;
}
