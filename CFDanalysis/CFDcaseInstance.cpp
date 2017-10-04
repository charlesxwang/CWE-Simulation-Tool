/*********************************************************************************
**
** Copyright (c) 2017 The Regents of the University of California
**
** Redistribution and use in source and binary forms, with or without modification,
** are permitted provided that the following conditions are met:
**
** 1. Redistributions of source code must retain the above copyright notice, this
** list of conditions and the following disclaimer.
**
** 2. Redistributions in binary form must reproduce the above copyright notice, this
** list of conditions and the following disclaimer in the documentation and/or other
** materials provided with the distribution.
**
** 3. Neither the name of the copyright holder nor the names of its contributors may
** be used to endorse or promote products derived from this software without specific
** prior written permission.
**
** THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
** EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
** OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
** SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
** INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
** TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
** BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
** CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
** IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
** SUCH DAMAGE.
**
***********************************************************************************/

// Contributors:
// Written by Peter Sempolinski, for the Natural Hazard Modeling Laboratory, director: Ahsan Kareem, at Notre Dame

#include "CFDcaseInstance.h"
#include "CFDanalysisType.h"

#include "../AgaveExplorer/remoteFileOps/fileoperator.h"
#include "../AgaveExplorer/remoteFileOps/filetreenode.h"
#include "../AgaveExplorer/remoteFileOps/easyboollock.h"
#include "../AgaveExplorer/remoteFileOps/joboperator.h"

#include "../AgaveClientInterface/filemetadata.h"
#include "../AgaveClientInterface/remotedatainterface.h"
#include "../AgaveClientInterface/remotejobdata.h"

#include "vwtinterfacedriver.h"

CFDcaseInstance::CFDcaseInstance(FileTreeNode * newCaseFolder, VWTinterfaceDriver * mainDriver):
    QObject((QObject *) mainDriver)
{
    caseFolder = newCaseFolder;
    theDriver = mainDriver;

    QObject::connect(theDriver->getFileHandler(), SIGNAL(newFileInfo()),
                     this, SLOT(underlyingFilesUpdated()));
    QObject::connect(caseFolder, SIGNAL(destroyed(QObject*)),
                     this, SLOT(caseFolderRemoved()));

    myLock = new EasyBoolLock(this);

    underlyingFilesUpdated(true);
}

CFDcaseInstance::CFDcaseInstance(CFDanalysisType * caseType, VWTinterfaceDriver *mainDriver):
    QObject((QObject *) mainDriver)
{
    myType = caseType;
    theDriver = mainDriver;

    QObject::connect(theDriver->getFileHandler(), SIGNAL(newFileInfo()),
                     this, SLOT(underlyingFilesUpdated()));

    myLock = new EasyBoolLock(this);
}

bool CFDcaseInstance::isDefunct()
{
    return defunct;
}

CaseState CFDcaseInstance::getCaseState()
{
    if (defunct) return CaseState::DEFUNCT;
    if (myLock->lockClosed()) return CaseState::AGAVE_INVOKE;

    if (caseFolder == NULL) return CaseState::LOADING;
    if (myType == NULL) return CaseState::LOADING;
    if (caseFolder->childIsUnloaded()) return CaseState::LOADING;

    if (caseFolder->getChildNodeWithName(".varStore") == NULL) return CaseState::INVALID;
    if (caseFolder->getChildNodeWithName(".varStore")->getFileBuffer() == NULL) return CaseState::LOADING;
    if (myType == NULL) return CaseState::INVALID;

    QMap<QString, StageState> stages = getStageStates();
    for (auto itr = stages.cbegin(); itr != stages.cend(); itr++)
    {
        if ((*itr) == StageState::LOADING)
        {
            return CaseState::LOADING;
        }
        if ((*itr) == StageState::ERROR)
        {
            return CaseState::ERROR;
        }
    }
    return CaseState::READY;
}

QString CFDcaseInstance::getCaseFolder()
{
    QString ret;
    if (caseFolder == NULL) return ret;
    return caseFolder->getFileData().getFullPath();
}

QString CFDcaseInstance::getCaseName()
{
    QString ret;
    if (caseFolder == NULL) return ret;
    return caseFolder->getFileData().getFileName();
}

CFDanalysisType * CFDcaseInstance::getMyType()
{
    if (defunct) return NULL;
    return myType;
}

QMap<QString, QString> CFDcaseInstance::getCurrentParams()
{
    QMap<QString, QString> ret;
    if (defunct) return ret;
    if (caseFolder == NULL) return ret;
    if (caseFolder->childIsUnloaded()) return ret;
    FileTreeNode * vars = caseFolder->getChildNodeWithName(".varStore");
    if (vars == NULL) return ret;

    QByteArray * rawVars = vars->getFileBuffer();
    if (rawVars == NULL) return ret;

    QJsonDocument varDoc = QJsonDocument::fromJson(*rawVars);

    if (varDoc.isNull()) return ret;

    QJsonObject varsList = varDoc.object().value("vars").toObject();

    for (auto itr = varsList.constBegin(); itr != varsList.constEnd(); itr++)
    {
        ret.insert(itr.key(),(*itr).toString());
    }

    return ret;
}

QMap<QString, StageState> CFDcaseInstance::getStageStates()
{
    QMap<QString, StageState> ret;
    if (defunct)
    {
        return ret;
    }
    if (caseFolder == NULL)
    {
        ret.insert("mesh", StageState::LOADING);
        ret.insert("sim", StageState::LOADING);
        return ret;
    }

    //Check job handler for running tasks on this folder
    QMap<QString, RemoteJobData> jobs = theDriver->getJobHandler()->getRunningJobs();

    for (auto itr = jobs.cbegin(); itr != jobs.cend(); itr++)
    {
        QString appName = (*itr).getApp();
        QMap<QString, QString> appParams = (*itr).getParams();

        if (appParams.contains("directory"))
        {
            QString appFolder = appParams.value("directory");
            if (caseFolder->folderNameMatches(appFolder))
            {
                if (appName.contains("cwe-sim"))
                {
                    ret.insert("mesh", StageState::FINISHED);
                    ret.insert("sim", StageState::RUNNING);
                    return ret;
                }
                if (appName.contains("cwe-exec-serial"))
                {
                    ret.insert("mesh", StageState::RUNNING);
                    ret.insert("sim", StageState::UNRUN);
                    return ret;
                }
                if (appName.contains("cwe-update"))
                {
                    ret.insert("mesh", StageState::LOADING);
                    ret.insert("sim", StageState::LOADING);
                    return ret;
                }
                if (appName.contains("cwe-delete"))
                {
                    ret.insert("mesh", StageState::LOADING);
                    ret.insert("sim", StageState::LOADING);
                    return ret;
                }
            }
        }
    }

    //Check known files for expected result files
    QJsonObject stages = myType->getRawConfig()->object().value("stages").toObject();

    for (auto itr = stages.constBegin(); itr != stages.constEnd(); itr++)
    {
        QString stageKey = itr.key();

        QString stageExpect = (*itr).toObject().value("expected").toString();

        if (stageExpect == "<NUMERICAL>")
        {
            if (caseFolder->childIsUnloaded())
            {
                ret.insert(stageKey, StageState::LOADING);
            }
            else
            {
                QList<FileTreeNode *> childList = caseFolder->getChildList();

                bool hasNumber = false;
                for (auto itr = childList.cbegin(); (itr != childList.cend()) && (hasNumber == false); itr++)
                {
                    QString nameToCheck = (*itr)->getFileData().getFileName();
                    bool ok = false;

                    int intval = nameToCheck.toInt(&ok);

                    if (ok && (intval > 0))
                    {
                        hasNumber = true;
                    }
                }

                if (hasNumber)
                {
                    ret.insert(stageKey, StageState::FINISHED);
                }
                else
                {
                    ret.insert(stageKey, StageState::UNRUN);
                }
            }
        }
        else
        {
            QStringList foldersToSearch = stageExpect.split('/');
            FileTreeNode * currentNode = caseFolder;
            bool foundData = false;
            for (auto itr = foldersToSearch.cbegin(); (itr != foldersToSearch.cend()) && (foundData == false); itr++)
            {
                QString folderName = (*itr);
                if (currentNode->childIsUnloaded())
                {
                    ret.insert(stageKey, StageState::LOADING);
                    foundData = true;
                }
                else
                {
                    currentNode = currentNode->getChildNodeWithName(folderName);
                    if (currentNode == NULL)
                    {
                        ret.insert(stageKey, StageState::UNRUN);
                        foundData = true;
                    }
                }
            }
            if (foundData == false)
            {
                ret.insert(stageKey, StageState::FINISHED);
            }
        }
    }
    return ret;
}

void CFDcaseInstance::forceInfoRefresh()
{
    if (defunct) return;

    if (caseFolder == NULL) return;

    //Enact LS of case folder itself
    FileOperator * fileHandle = theDriver->getFileHandler();
    fileHandle->enactFolderRefresh(caseFolder);

    //If case type known:
    //Enact LS of folders from list of check files in configuration
    QJsonObject stages = myType->getRawConfig()->object().value("stages").toObject();

    for (auto itr = stages.constBegin(); itr != stages.constEnd(); itr++)
    {
        QString stageKey = itr.key();

        QString stageExpect = (*itr).toObject().value("expected").toString();

        if (stageExpect == "<NUMERICAL>")
        {
            continue;
        }

        QStringList foldersToSearch = stageExpect.split('/');
        FileTreeNode * currentNode = caseFolder;
        for (auto itr = foldersToSearch.cbegin(); (itr != foldersToSearch.cend()) && (currentNode != NULL); itr++)
        {
            QString folderName = (*itr);
            if (currentNode->childIsUnloaded())
            {
                currentNode = NULL;
            }
            else
            {
                currentNode = currentNode->getChildNodeWithName(folderName);
            }

            if (currentNode != NULL)
            {
                fileHandle->enactFolderRefresh(currentNode);
            }
        }
    }

    //Enact buffer download of .varStore
    FileTreeNode varNode = caseFolder->getChildNodeWithName(".varStore");
    if (varNode != NULL)
    {
        fileHandle->sendDownloadBuffReq(varNode);
    }
}

void CFDcaseInstance::createCase(QString newName, FileTreeNode * containingFolder)
{
    if (defunct) return;
    if (!myLock->checkAndClaim()) return;
    if (!expectedNewCaseFolder.isEmpty()) return;
    if (caseFolder != NULL) return;

    expectedNewCaseFolder = containingFolder->getFileData().getFullPath();
    expectedNewCaseFolder = expectedNewCaseFolder.append("/");
    expectedNewCaseFolder = expectedNewCaseFolder.append(newName);

    QMultiMap<QString, QString> rawParams;
    rawParams.insert("newFolder", newName);
    rawParams.insert("template", myType->getInternalName());

    RemoteDataInterface * remoteConnect = theDriver->getDataConnection();
    RemoteDataReply * jobHandle = remoteConnect->runRemoteJob("cwe-create", rawParams, containingFolder->getFileData().getFullPath());

    if (jobHandle == NULL)
    {
        myLock->release();
        defunct = true;
    }
    QObject::connect(jobHandle, SIGNAL(haveJobReply(RequestState,QJsonDocument*)),
                     this, SLOT(agaveAppDone()));
}

void CFDcaseInstance::changeParameters(QMap<QString, QString> paramList)
{
    if (defunct) return;
    if (caseFolder == NULL) return;
    if (!myLock->checkAndClaim()) return;

    FileTreeNode * varStore = caseFolder->getChildNodeWithName(".varStore");

    varStore->setFileBuffer(NULL);

    QMultiMap<QString, QString> rawParams;
    QString paramString;

    qDebug("sending ...\n");
    for (auto itr = paramList.cbegin(); itr != paramList.cend(); itr++)
    {
        qDebug("%s: %s", qPrintable(itr.key()), qPrintable(*itr));
        paramString += (itr.key());
        paramString += " ";
        paramString += (*itr);
        paramString += " ";
    }

    rawParams.insert("params", paramString);

    RemoteDataInterface * remoteConnect = theDriver->getDataConnection();
    RemoteDataReply * jobHandle = remoteConnect->runRemoteJob("cwe-update", rawParams, caseFolder->getFileData().getFullPath());

    if (jobHandle == NULL)
    {
        myLock->release();
        defunct = true;
    }
    QObject::connect(jobHandle, SIGNAL(haveJobReply(RequestState,QJsonDocument*)),
                     this, SLOT(agaveAppDone()));
}

void CFDcaseInstance::mesh(FileTreeNode * geoFile)
{
    if (defunct) return;
    if (!myLock->checkAndClaim()) return;

    QMultiMap<QString, QString> rawParams;
    rawParams.insert("action", "mesh");
    rawParams.insert("inFile", geoFile->getFileData().getFullPath());

    RemoteDataInterface * remoteConnect = theDriver->getDataConnection();
    RemoteDataReply * jobHandle = remoteConnect->runRemoteJob("cwe-exec-serial", rawParams, caseFolder->getFileData().getFullPath());

    if (jobHandle == NULL)
    {
        myLock->release();
        defunct = true;
    }
    QObject::connect(jobHandle, SIGNAL(haveJobReply(RequestState,QJsonDocument*)),
                     this, SLOT(agaveAppDone()));
}

void CFDcaseInstance::rollBack(QString stageToDelete)
{
    if (defunct) return;
    if (!myLock->checkAndClaim()) return;

    QMultiMap<QString, QString> rawParams;
    rawParams.insert("step", stageToDelete);

    RemoteDataInterface * remoteConnect = theDriver->getDataConnection();
    RemoteDataReply * jobHandle = remoteConnect->runRemoteJob("cwe-delete", rawParams, caseFolder->getFileData().getFullPath());

    if (jobHandle == NULL)
    {
        myLock->release();
        defunct = true;
    }
    QObject::connect(jobHandle, SIGNAL(haveJobReply(RequestState,QJsonDocument*)),
                     this, SLOT(agaveAppDone()));
}

void CFDcaseInstance::openFOAM()
{
    if (defunct) return;
    if (!myLock->checkAndClaim()) return;

    QMultiMap<QString, QString> rawParams;
    QMap<QString, QString> caseParams = getCurrentParams();

    //TODO: When new Agave app available, let backend determine solver
    if (caseParams.contains("turbModel") && (caseParams.value("turbModel") != "Laminar"))
    {
        rawParams.insert("solver", "icoFoam");
    }
    else
    {
        rawParams.insert("solver", "pisoFoam");
    }

    RemoteDataInterface * remoteConnect = theDriver->getDataConnection();
    RemoteDataReply * jobHandle = remoteConnect->runRemoteJob("cwe-sim", rawParams, caseFolder->getFileData().getFullPath());

    if (jobHandle == NULL)
    {
        myLock->release();
        defunct = true;
    }
    QObject::connect(jobHandle, SIGNAL(haveJobReply(RequestState,QJsonDocument*)),
                     this, SLOT(agaveAppDone()));
}

void CFDcaseInstance::postProcess()
{
    if (defunct) return;
    if (!myLock->checkAndClaim()) return;

    QMultiMap<QString, QString> rawParams;
    rawParams.insert("action", "post");

    RemoteDataInterface * remoteConnect = theDriver->getDataConnection();
    RemoteDataReply * jobHandle = remoteConnect->runRemoteJob("cwe-exec-serial", rawParams, caseFolder->getFileData().getFullPath());

    if (jobHandle == NULL)
    {
        myLock->release();
        defunct = true;
    }
    QObject::connect(jobHandle, SIGNAL(haveJobReply(RequestState,QJsonDocument*)),
                     this, SLOT(agaveAppDone()));
}

void CFDcaseInstance::killCaseConnection()
{
    defunct = true;
    emit detachCase();
    this->deleteLater();
}

void CFDcaseInstance::underlyingFilesUpdated(bool forceRefresh)
{
    if (defunct) return;

    bool needRefresh = forceRefresh;

    if (caseFolder == NULL)
    {
        if (!expectedNewCaseFolder.isEmpty())
        {
            //If caseFolder is null, try to find expectedNewCaseFolder as file node

            FileOperator * fileHandle = theDriver->getFileHandler();
            caseFolder = fileHandle->getNodeFromName(expectedNewCaseFolder);

            if (caseFolder == NULL)
            {
                fileHandle->lsClosestNode(expectedNewCaseFolder);
            }
            else
            {
                //If setting file node, connect caseFolderRemoved slot
                expectedNewCaseFolder.clear();
                QObject::connect(caseFolder, SIGNAL(destroyed(QObject*)),
                                 this, SLOT(caseFolderRemoved()));
                needRefresh = true;
            }
        }
    }

    if (caseFolder != NULL)
    {
        if (myType == NULL)
        {
            //If caseFolder exists, try to determine myType from varStore
            FileTreeNode varFile = caseFolder->getChildNodeWithName(".varStore");
            if (varFile != NULL)
            {
                QByteArray * varStore = varFile.getFileBuffer();
                if (varStore != NULL)
                {
                    QJsonDocument varDoc = QJsonDocument::fromJson(varDoc);
                    QString templateName = varDoc.object().value("type").toString();
                    if (!templateName.isEmpty())
                    {
                        QList<CFDanalysisType *> * templates = theDriver->getTemplateList();
                        for (auto itr = templates->cbegin(); (itr != templates->cend()) && (myType == NULL); itr++)
                        {
                            if (templateName == (*itr)->getInternalName())
                            {
                                myType = (*itr);
                                needRefresh = true;
                            }
                        }
                    }
                }
            }
        }
    }

    emitNewState();

    if (oldState == CaseState::LOADING)
    {
        needRefresh = true;
    }

    if (needRefresh)
    {
        forceInfoRefresh();
    }
}

void CFDcaseInstance::caseFolderRemoved()
{
    killCaseConnection();
    theDriver->setCurrentCase(NULL);
}

void CFDcaseInstance::remoteAppDone()
{
    if (defunct) return;
    myLock->release();

    underlyingFilesUpdated(true);
}

void CFDcaseInstance::emitNewState()
{
    if (defunct == true) return;
    CaseState newState = getCaseState();
    if (newState == oldState) return;
    emit haveNewState(oldState, newState);
    oldState = newState;
}
