#include "system/iplSaveDataManager.h"

#include "system/iplChannelManager.h"

#include "iplSystem.h"

#include "iplUtility.h"

#include <revolution.h>
#include <revolution/sc.h>

#include <cstring>

#include "titledb.h"

#include "config.h"

namespace ipl {
    // clang-format off
    #define DISK_CHANNEL        CHANNEL_INFO(channel::PRIMARY_TYPE_DISK,    channel::SECONARY_TYPE_SYSTEM, SCENE_DISK_CHANNEL,   0)
    #define NIGAOE_CHANNEL      CHANNEL_INFO(channel::PRIMARY_TYPE_CHANNEL, channel::SECONARY_TYPE_SYSTEM, SCENE_NORMAL_CHANNEL, TITLE_NIGAOE_ALL)
    #define PHOTO_CHANNEL       CHANNEL_INFO(channel::PRIMARY_TYPE_CHANNEL, channel::SECONARY_TYPE_SYSTEM, SCENE_NORMAL_CHANNEL, TITLE_PHOTO_ALL)
    #define SHOPPING_CHANNEL    CHANNEL_INFO(channel::PRIMARY_TYPE_CHANNEL, channel::SECONARY_TYPE_SYSTEM, SCENE_NORMAL_CHANNEL, TITLE_SHOPPING_ALL)
    #define WEATHER_CHANNEL     CHANNEL_INFO(channel::PRIMARY_TYPE_CHANNEL, channel::SECONARY_TYPE_SYSTEM, SCENE_NORMAL_CHANNEL, TITLE_WEATHER_ALL)
    #define NEWS_CHANNEL        CHANNEL_INFO(channel::PRIMARY_TYPE_CHANNEL, channel::SECONARY_TYPE_SYSTEM, SCENE_NORMAL_CHANNEL, TITLE_NEWS_ALL)
    #define NO_CHANNEL          CHANNEL_INFO_NULL

    #define DEFAULT_CHANNEL_COUNT 6
    static const channel::SInfo cDefaultChanList/*[MAX_CHANNEL_PAGE][MAX_CHANNEL_INDEX]*/[MAX_CHANNEL_TOTAL] = {
        // Page 1
        //{
            /* =============================================================*/
            DISK_CHANNEL,   NIGAOE_CHANNEL, PHOTO_CHANNEL, SHOPPING_CHANNEL,
            /* =============================================================*/
            WEATHER_CHANNEL, NEWS_CHANNEL,   NO_CHANNEL,    NO_CHANNEL,
            /* =============================================================*/
            NO_CHANNEL,      NO_CHANNEL,     NO_CHANNEL,    NO_CHANNEL,
            /* =============================================================*/
        //},
        // Page 2
        //{
            /* =============================================================*/
            NO_CHANNEL,      NO_CHANNEL,     NO_CHANNEL,    NO_CHANNEL,
            /* =============================================================*/
            NO_CHANNEL,      NO_CHANNEL,     NO_CHANNEL,    NO_CHANNEL,
            /* =============================================================*/
            NO_CHANNEL,      NO_CHANNEL,     NO_CHANNEL,    NO_CHANNEL,
            /* =============================================================*/
        //},
        // Page 3
        //{
            /* =============================================================*/
            NO_CHANNEL,      NO_CHANNEL,     NO_CHANNEL,    NO_CHANNEL,
            /* =============================================================*/
            NO_CHANNEL,      NO_CHANNEL,     NO_CHANNEL,    NO_CHANNEL,
            /* =============================================================*/
            NO_CHANNEL,      NO_CHANNEL,     NO_CHANNEL,    NO_CHANNEL,
            /* =============================================================*/
        //},
        // Page 4
        //{
            /* =============================================================*/
            NO_CHANNEL,      NO_CHANNEL,     NO_CHANNEL,    NO_CHANNEL,
            /* =============================================================*/
            NO_CHANNEL,      NO_CHANNEL,     NO_CHANNEL,    NO_CHANNEL,
            /* =============================================================*/
            NO_CHANNEL,      NO_CHANNEL,     NO_CHANNEL,    NO_CHANNEL,
            /* =============================================================*/
        //},
    };
    // clang-format on

    static const char* csSavePath = "/title/00000001/00000002/data/iplsave.bin";

    namespace savedata {
        Manager::Manager(EGG::Heap* heap) {
            mpHeap = heap;
            mLastPrevPage = 0;
            mLastSDPrevPage = 0;
            mbBadSDPrevPage = true;

            mpFile = NULL;

            mpUpdatedFile = NULL;

            mbInit = false;

            mLastError = 0;
        }

        Manager::~Manager() {
            if (mpUpdatedFile) {
                delete mpUpdatedFile;
            }
        }

        void Manager::initManager() {
            mbInit = false;
            System::getTask3()->request(initManagerTask, this, NULL);
        }

        void Manager::setPrevPage(int prevPage) {
            mData.prevPage = prevPage;
        }

        void Manager::setChanInfo(int page, int index, const channel::SInfo& chanInfo) {
            mData.chanInfo[page][index] = chanInfo;
        }

        void Manager::setMemoSetting(const textinput::extend::savedata::MemoSetting& memoSetting) {
            mData.memoSetting = memoSetting;
        }

        nand::File* Manager::flushAsync(EGG::Heap* flushHeap) {
            int old = OSDisableInterrupts();

            NETMD5Sum md5;
            NETCalcMD5(md5, &mData, sizeof(mData) - NET_MD5_DIGEST_SIZE);
            memcpy(mData.MD5Sum, md5, NET_MD5_DIGEST_SIZE);

            nand::File* result = System::getNandManager()->writeAsync(flushHeap, csSavePath, &mData, sizeof(mData), NAND_PERM_USER);
            mpFile = result;

            OSRestoreInterrupts(old);

            return result;
        }

        BOOL Manager::isFinished(nand::File* file) {
            if (file->isFinished() || file->isFatalError()) {
                mpFile = NULL;
            }
            return file->isFinished();
        }

        // MWCC needs the input-region comparison computed inside the channel search.
        #pragma push
        #pragma opt_loop_invariants off
        ESTitleId Manager::hasChannel(ESTitleId titleId, int* outIndex, int* outPage) const {
            ESTitleId mask;
            if (ES_TITLE_TYPE(titleId) != 0) {
                mask = 0xFFFFFFFFFFFFFFFFULL;
            } else {
                mask = 0x00000000FFFFFFFFULL;
            }
            ESTitleId regionMask = mask & TITLE_NOREGION_MASK;
            int page = 0;
            do {
                for (int index = 0; index < MAX_CHANNEL_INDEX; index++) {
                    if (mData.chanInfo[page][index].primaryType == channel::PRIMARY_TYPE_CHANNEL) {
                        ESTitleId stored = ES_TITLE_ID(mData.chanInfo[page][index].titleType, mData.chanInfo[page][index].titleCode);
                        if ((stored & mask) == titleId ||
                            ((stored & regionMask) == (titleId & regionMask) && (ESTitleId)(u8)titleId == TITLE_REGION_ALL)) {
                            if (outIndex) {
                                *outIndex = page;
                            }
                            if (outPage) {
                                *outPage = index;
                            }
                            return stored;
                        }
                    }
                }
                page++;
            } while (page < MAX_CHANNEL_PAGE);
            return 0;
        }

        #pragma pop

        int Manager::getNumValidChannel() const {
            // MWCC needs this page view to preserve the original channel-base calculation.
            const ChannelSlot* channelSlot;
            int channelsPerPage = MAX_CHANNEL_INDEX;
            const ChannelPage* channelPage;
            int index;
            int count = 0;
            int page;
            u8 primaryType;
            s32 sceneID;
            for (page = 0; page < MAX_CHANNEL_PAGE; page++) {
                channelPage = (const ChannelPage*)((const u8*)this + page * sizeof(mData.chanInfo[page]));
                for (index = 0; index < channelsPerPage; index++) {
                    channelSlot = &channelPage->slots[index];
                    primaryType = channelSlot->valid;
                    if (primaryType != 0) {
                        sceneID = channelSlot->titleId;
                        if (sceneID != 0) {
                            count++;
                        }
                    }
                }
            }
            return count;
        }

        BOOL Manager::isResetAcceptable() {
            if (mpFile == NULL || mpFile->isFinished() || mpFile->isFatalError()) {
                return TRUE;
            } else {
                return FALSE;
            }
        }

        void Manager::initManagerTask(void* work) {
            Manager* manager = static_cast<Manager*>(work);
            s32 ret;

            u32 fileLen = sizeof(Data);
            BOOL bCreateNew = FALSE;
            bool bNeedFlush = false;

            ES_SetUid(SYSMENU_TITLE_ID);

            if (manager->mpUpdatedFile == NULL) {
                NANDFileInfo fileInfo;
                ret = nand::wrapper::Open(csSavePath, &fileInfo, NAND_ACCESS_RW);

                if (manager->nand_error_handling(ret)) {
                    u32 fileLenCheck;
                    ret = nand::wrapper::GetLength(&fileInfo, &fileLenCheck);
                    manager->nand_error_handling(ret);

                    if (fileLenCheck == 0) {
                        // Create a new file if blank
                        bCreateNew = TRUE;
                    } else {
                        // Read header
                        ret = nand::wrapper::Read(&fileInfo, &manager->mData, 0x20);
                        manager->nand_error_handling(ret);

                        if (manager->mLastError == NAND_RESULT_AUTHENTICATION || manager->mLastError == NAND_RESULT_ECC_CRIT) {
                            // Access to file is broken, create new
                            ret = nand::wrapper::Delete(csSavePath);
                            manager->nand_error_handling(ret);
                            bCreateNew = TRUE;
                        } else {
                            if (manager->mData.version > SAVEDATA_VERSION) {
                                // Save data too high, create new
                                bCreateNew = TRUE;
                            } else {
                                if (!NAND_CHECK_MAGIC4(manager->mData.sig, 'R', 'I', 'P', 'L')) {
                                    // Signature incorrect, create new
                                    bCreateNew = TRUE;
                                } else {
                                    ret = nand::wrapper::GetLength(&fileInfo, &fileLen);
                                    if (ret != NAND_RESULT_OK || manager->mData.fileSize < 0x20 || manager->mData.fileSize > fileLen) {
                                        // Invalid filesize, create new
                                        bCreateNew = TRUE;
                                    } else {
                                        fileLen = manager->mData.fileSize;

                                        ret = nand::wrapper::Seek(&fileInfo, 0, NAND_SEEK_BEG);
                                        manager->nand_error_handling(ret);
                                        ret = nand::wrapper::Read(&fileInfo, &manager->mData, fileLen);
                                        manager->nand_error_handling(ret);

                                        if (manager->mLastError == NAND_RESULT_AUTHENTICATION || manager->mLastError == NAND_RESULT_ECC_CRIT) {
                                            // Access to file is broken, create new
                                            ret = nand::wrapper::Delete(csSavePath);
                                            manager->nand_error_handling(ret);
                                            bCreateNew = TRUE;
                                        } else {
                                            NETMD5Sum md5;
                                            NETCalcMD5(md5, &manager->mData, fileLen - NET_MD5_DIGEST_SIZE);

                                            u8* data = (u8*)manager;
                                            u32* loopFileLen = &fileLen;
                                            // MWCC needs volatile MD5 reads to preserve the original byte-load schedule.
                                            volatile NETMD5Sum& md5Ref = md5;
                                            u32 i = 0;
                                            while (i < NET_MD5_DIGEST_SIZE) {
                                                u8 md5Byte = md5Ref[i];
                                                u32 offset = i;
                                                offset += *loopFileLen;
                                                u8* fileByteAddress = (u8*)(offset + (u32)data + NET_MD5_DIGEST_SIZE);
                                                u32 fileByte = *fileByteAddress;
                                                if (md5Byte != fileByte) {
                                                    // Invalid MD5 sum, create new
                                                    bCreateNew = TRUE;
                                                    break;
                                                }
                                                i++;
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                    ret = nand::wrapper::Close(&fileInfo);
                    manager->nand_error_handling(ret);
                } else {
                    if (manager->mLastError == NAND_RESULT_NOEXISTS) {
                        // Create a new file if it does not exist
                        bCreateNew = TRUE;
                    } else {
                        // Otherwise something is wrong
                        IPLErrorLogAndDisplay(MESG_ERR_FILE, "NAND", manager->mLastError, 450);
                    }
                }

                if (bCreateNew) {
                    manager->setDefaultSaveData();
                    bNeedFlush = TRUE;
                } else {
                    u32 oldVersion = manager->mData.version;
                    if (oldVersion < SAVEDATA_VERSION) {
                        manager->updateVersion(SAVEDATA_VERSION, oldVersion);
                        bNeedFlush = TRUE;
                    }
                }

                // Previous page of ChannelSelect
                if (manager->mData.prevPage != 0) {
                    manager->mLastPrevPage = manager->mData.prevPage;
                    bNeedFlush = TRUE;
                    manager->mData.prevPage = 0;
                }

                // Prevous page on SDChannelSelect
                if (manager->mData.prevSDPage >= 0 && manager->mData.prevSDPage < 20) {
                    if (manager->mbBadSDPrevPage) {
                        manager->mLastSDPrevPage = manager->mData.prevSDPage;
                        manager->mbBadSDPrevPage = false;
                    }
                }
            }

            bool flush = bNeedFlush | manager->updateChanInfos();
            if (flush) {
                if (manager->mpUpdatedFile) {
                    delete manager->mpUpdatedFile;
                }

                manager->mpUpdatedFile = manager->flushAsync(manager->mpHeap);
                manager->mpFile = NULL;
            }

            manager->mbInit = true;
        }

        void Manager::setDefaultSaveData() {
            setDefaultHeader();
            memcpy(mData.chanInfo, cDefaultChanList, MAX_CHANNEL_INFO_SIZE);
            setDefaultKeyboard();
            setDefaultTVRC();
            setDefaultSDMenu();

            for (int i = 0; i < 0x0C; i++) {
                mData.padding[i] = 0;
            }
        }

        void Manager::setDefaultHeader() {
            mData.sig[0] = 'R';
            mData.sig[1] = 'I';
            mData.sig[2] = 'P';
            mData.sig[3] = 'L';
            mData.fileSize = sizeof(mData);
            mData.version = SAVEDATA_VERSION;
            mData.prevPage = 0;
        }

        void Manager::setDefaultKeyboard() {
            mData.memoSetting.uRevisionAndType = 0x11;

            mData.memoSetting.uDictionary = 0xFA;
            mData.memoSetting.uPredictOnOff = 0x00;
            mData.memoSetting.uSignPage = 0x00;

            mData.memoSetting.uKeitaiUpperCaseJP = TRUE;
            mData.memoSetting.uKeitaiInputMode = 0x02;

            mData.memoSetting.uQwertyABC = FALSE;

            mData.memoSetting.uABCInputMode = 0x01;
            mData.memoSetting.uAIUInputMode = 0x00;

            mData.memoSetting.uNumLockOff = FALSE;

            mData.memoSetting.uReserve2 = 0x00;
            mData.memoSetting.uReserve3 = 0x00;
            mData.memoSetting.uReserve4 = 0x00;
        }

        void Manager::setDefaultTVRC() {
            mData.tvrcData[0] = 0;
            mData.tvrcData[1] = 0;
            mData.tvrcData[2] = 0;
            mData.tvrcData[3] = 0;
        }

        BOOL Manager::updateChanInfos() {
            u32 titleCount;
            ES_ListTitlesOnCard(NULL, &titleCount);

            u64* titleIds = new (mpHeap, -DEFAULT_ALIGN) u64[titleCount];

            s32 ret = ES_ListTitlesOnCard(titleIds, &titleCount);
            if (ret != ES_ERR_OK) {
                IPLErrorLogAndDisplay(MESG_ERR_FILE, "ES", ret, 637);
            }

            mbPhoto2 = false;
            mbPhoto2Check = false;
            mbPhotoMP3 = false;

            mPhotoId = TITLE_PHOTO;

            deleteInvalidTitle(titleIds, titleCount);
            checkSpecialTitles(titleIds, titleCount);
            checkTmpTitle(titleIds, titleCount);

            ESTitleId prioritizedTitleIds[MAX_CHANNEL_TOTAL] = {0};

            makePriorTitleIDList(prioritizedTitleIds, titleIds, titleCount);
            integrateTitleIDList(prioritizedTitleIds, titleIds, titleCount);

            delete[] titleIds;

            return doUpdateChanInfos(prioritizedTitleIds);
        }

        void Manager::deleteInvalidTitle(ESTitleId* titleIds, u32 titleCount) {
            for (int i = 0; i < titleCount; i++) {
                s32 ret;
                if ((ESTitleId32)ES_TITLE_TYPE(titleIds[i]) != TITLE_TYPE_CHANNEL &&
                    (ESTitleId32)ES_TITLE_TYPE(titleIds[i]) != TITLE_TYPE_SYSTEM_CHANNEL &&
                    (ESTitleId32)ES_TITLE_TYPE(titleIds[i]) != TITLE_TYPE_DISC &&
                    (ESTitleId32)ES_TITLE_TYPE(titleIds[i]) != TITLE_TYPE_DISC_CHANNEL &&
                    (ESTitleId32)ES_TITLE_TYPE(titleIds[i]) != TITLE_TYPE_UNK6 && (ESTitleId32)ES_TITLE_TYPE(titleIds[i]) != TITLE_TYPE_UNK3) {
                    titleIds[i] = TITLE_NULL;
                } else {
                    ESTmdView* tmd = NULL;
                    ret = utility::ESMisc::GetTmdView(mpHeap, titleIds[i], &tmd);
                    if (ret == ES_ERR_OK) {
                        if (tmd->contents[0].size == 0x40 && TITLE_NO_REGION(titleIds[i]) != TITLE_PHOTO) {
                            titleIds[i] = TITLE_NULL;
                        }
                    }
                    if (tmd) {
                        mpHeap->free(tmd);
                    }

                    if (TITLE_NO_REGION(titleIds[i]) == TITLE_PHOTO) {
                        ESTmdView* tmd = NULL;
                        ret = utility::ESMisc::GetTmdView(mpHeap, titleIds[i], &tmd);
                        if (ret == ES_ERR_OK && tmd->head.titleVersion == 65280) {
                            mbPhotoMP3 = true;
                        }
                        if (tmd) {
                            mpHeap->free(tmd);
                        }
                    } else if (TITLE_NO_REGION(titleIds[i]) == TITLE_PHOTO_2) {
                        if (checkValidApp(titleIds[i])) {
                            mbPhoto2 = true;
                        }
                    } else if (TITLE_NO_REGION(titleIds[i]) == TITLE_PHOTO_2_CHECK) {
                        if (checkValidApp(titleIds[i])) {
                            mbPhoto2Check = true;
                        }
                    }
                }
            }
        }

        void Manager::checkSpecialTitles(ESTitleId* titleIds, u32 titleCount) {
            if (mbPhoto2 || mbPhoto2Check) {
                for (int i = 0; i < titleCount; i++) {
                    if (titleIds[i]) {
                        ESTitleId titleIdNoRegion = TITLE_NO_REGION(titleIds[i]);
                        if (mbPhoto2 && mbPhoto2Check) {
                            if (titleIdNoRegion == TITLE_PHOTO) {
                                titleIds[i] = 0;
                            } else if (titleIdNoRegion == TITLE_PHOTO_2_CHECK) {
                                titleIds[i] = 0;
                            }
                        } else if (!mbPhoto2 && mbPhoto2Check) {
                            if (titleIdNoRegion == TITLE_PHOTO) {
                                titleIds[i] = 0;
                            }
                        } else if (mbPhoto2 && !mbPhoto2Check && mbPhotoMP3) {
                            if (titleIdNoRegion == TITLE_PHOTO) {
                                titleIds[i] = 0;
                            }
                        } else if (mbPhoto2 && !mbPhoto2Check && !mbPhotoMP3) {
                            if (titleIdNoRegion == TITLE_PHOTO_2) {
                                titleIds[i] = 0;
                            }
                        }
                    }
                }

                if (mbPhoto2 && mbPhoto2Check) {
                    mPhotoId = TITLE_PHOTO_2;
                } else if (!mbPhoto2 && mbPhoto2Check) {
                    mPhotoId = TITLE_PHOTO_2_CHECK;
                } else if (mbPhoto2 && !mbPhoto2Check && mbPhotoMP3) {
                    mPhotoId = TITLE_PHOTO_2;
                } else if (mbPhoto2 && !mbPhoto2Check && !mbPhotoMP3) {
                    mPhotoId = TITLE_PHOTO;
                }
            }
        }

        void Manager::checkTmpTitle(ESTitleId* titleIds, u32 titleCount) {
            bool foundTmp = false;

            ESTitleId tmpId = SCGetTmpTitleID();
            if (tmpId) {
                for (int i = 0; i < titleCount; i++) {
                    if (titleIds[i] == tmpId) {
                        titleIds[i] = 0;
                        foundTmp = true;
                    }
                }

                if (foundTmp) {
                    if (!checkValidApp(tmpId)) {
                        SCSetTmpTitleID(TITLE_NULL);
                        SCFlush();
                    }
                } else {
                    SCSetTmpTitleID(TITLE_NULL);
                    SCFlush();
                }
            }
        }

#if defined(__MWERKS__) || defined(RVL_WEB)

        // MWCC needs IRO disabled to recompute title-list addresses at each use.
#pragma push
#pragma ppc_iro_level 0
        void Manager::makePriorTitleIDList(ESTitleId* titleIdsOut, ESTitleId* titleIdsIn, u32 titleCount) {
            int outputIndex;
            for (int page = 0; page < MAX_CHANNEL_PAGE; page++) {
                for (int index = 0; index < MAX_CHANNEL_INDEX; index++) {
                    if (mData.chanInfo[page][index].primaryType == channel::PRIMARY_TYPE_CHANNEL) {
                        outputIndex = index + page * MAX_CHANNEL_INDEX;
                        ESTitleId titleId = ES_TITLE_ID(mData.chanInfo[page][index].titleType, mData.chanInfo[page][index].titleCode);
                        for (u32 inputIndex = 0; inputIndex < titleCount; inputIndex++) {
                            if (titleIdsIn[inputIndex] == TITLE_NULL) {
                                continue;
                            }
                            if (titleIdsOut[outputIndex] != TITLE_NULL) {
                                titleId = titleIdsOut[outputIndex];
                            } else {
                                ESTitleId titleCode = TITLE_NO_REGION(titleId);
                                if (titleCode == TITLE_PHOTO || titleCode == TITLE_PHOTO_2 || titleCode == TITLE_PHOTO_2_CHECK) {
                                    if (titleCode != mPhotoId) {
                                        titleId = mPhotoId | TITLE_REGION_ALL;
                                    }
                                }
                            }
                            int match = isEqualChannel(titleId, titleIdsIn[inputIndex]);
                            if (match == -2 || match == 0) {
                                if (checkValidApp(titleId)) {
                                    titleIdsOut[outputIndex] = titleId;
                                }
                                titleIdsIn[inputIndex] = TITLE_NULL;
                            } else if (match == 1) {
                                if (checkValidApp(titleIdsIn[inputIndex])) {
                                    titleIdsOut[outputIndex] = titleIdsIn[inputIndex];
                                }
                                titleIdsIn[inputIndex] = TITLE_NULL;
                            }
                        }
                    }
                }
            }
        }
#pragma pop

        void Manager::integrateTitleIDList(ESTitleId* titleIdsOut, ESTitleId* titleIdsIn, u32 titleCount) {
            ESTitleId temporaryTitleIds[0x36] = {0};
            int availableCount = getAvailableNumInList(titleIdsOut, 0x30);
            if (availableCount != 0) {
                makeTmpList(temporaryTitleIds, availableCount, titleIdsIn, titleCount);
                moveTitleTmpToPrior(titleIdsOut, temporaryTitleIds);
            }
        }

        void Manager::makeTmpList(ESTitleId* titleIdsOut, u32 availableCount, ESTitleId* titleIdsIn, u32 titleCount) {
            int appendIndex = DEFAULT_CHANNEL_COUNT;
            u32 addedCount = 0;
            for (int inputIndex = 0; inputIndex < titleCount; inputIndex++) {
                if (titleIdsIn[inputIndex] != TITLE_NULL) {
                    if (checkValidApp(titleIdsIn[inputIndex])) {
                        int match = -1;
                        for (int outputIndex = 0; outputIndex < appendIndex; outputIndex++) {
                            match = isEqualChannel(titleIdsOut[outputIndex], titleIdsIn[inputIndex]);
                            if (match == -2 || match == 0) {
                                break;
                            }
                            if (match == 1) {
                                titleIdsOut[outputIndex] = titleIdsIn[inputIndex];
                                break;
                            }
                        }
                        if (match != -1) {
                            titleIdsIn[inputIndex] = TITLE_NULL;
                            continue;
                        }
                        int defaultIndex = isDefaultChannel(ES_TITLE_TYPE(titleIdsIn[inputIndex]), ES_TITLE_CODE(titleIdsIn[inputIndex]));
                        if (defaultIndex == -1) {
                            titleIdsOut[appendIndex++] = titleIdsIn[inputIndex];
                        } else {
                            titleIdsOut[defaultIndex] = titleIdsIn[inputIndex];
                        }
                        addedCount++;
                    }
                    titleIdsIn[inputIndex] = TITLE_NULL;
                    if (addedCount >= availableCount) {
                        break;
                    }
                }
            }
        }

        void Manager::moveTitleTmpToPrior(ESTitleId* titleIdsOut, const ESTitleId* titleIdsIn) {
            for (int titleIndex = 0; titleIndex < 0x36; titleIndex++) {
                if (titleIdsIn[titleIndex] != TITLE_NULL) {
                    int availableIndex = getAvailableInList(titleIdsOut, 0x30);
                    if (availableIndex == -1) {
                        return;
                    }
                    titleIdsOut[availableIndex] = titleIdsIn[titleIndex];
                }
            }
        }

        // MWCC needs IRO disabled to retain separate indexed channel address calculations.
#pragma push
#pragma ppc_iro_level 0
        BOOL Manager::doUpdateChanInfos(ESTitleId* titleIds) {
            BOOL changed = FALSE;
            for (int page = 0; page < MAX_CHANNEL_PAGE; page++) {
                for (int index = 0; index < MAX_CHANNEL_INDEX; index++) {
                    if (mData.chanInfo[page][index].primaryType != channel::PRIMARY_TYPE_DISK) {
                        ESTitleId titleId = ES_TITLE_ID(mData.chanInfo[page][index].titleType, mData.chanInfo[page][index].titleCode);
                        int titleIndex = index + page * MAX_CHANNEL_INDEX;
                        if (titleId != titleIds[titleIndex]) {
                            if (titleIds[titleIndex] == TITLE_NULL) {
                                memset(&mData.chanInfo[page][index], 0, sizeof(channel::SInfo));
                            } else {
                                mData.chanInfo[page][index].primaryType = channel::PRIMARY_TYPE_CHANNEL;
                                mData.chanInfo[page][index].secondaryType = channel::SECONARY_TYPE_NORMAL;
                                mData.chanInfo[page][index].reserved[0] = 0;
                                mData.chanInfo[page][index].reserved[1] = 0;
                                mData.chanInfo[page][index].sceneID = SCENE_NORMAL_CHANNEL;
                                mData.chanInfo[page][index].titleType = ES_TITLE_TYPE(titleIds[titleIndex]);
                                mData.chanInfo[page][index].titleCode = ES_TITLE_CODE(titleIds[titleIndex]);
                            }
                            changed = TRUE;
                        }
                    }
                }
            }
            return changed;
        }
#pragma pop
#endif

        BOOL Manager::checkValidApp(ESTitleId titleId) {
            BOOL result = TRUE;

            u32 tikCount = 0;
            s32 ret = ES_GetTicketViews(titleId, NULL, &tikCount);
            if (ret < ES_ERR_OK || tikCount == 0) {
                return FALSE;
            }

            if (!utility::ESMisc::PrivateContentsExist(titleId)) {
                return FALSE;
            }

            ESTmdView* tmd = NULL;
            ret = utility::ESMisc::GetTmdView(mpHeap, titleId, &tmd);
            if (ret != ES_ERR_OK && ret != ES_ERR_NO_TMD_FILE_FOUND) {
                IPLErrorLogAndDisplay(MESG_ERR_FILE, "ES", ret, 1138);
            }

            if (ret == ES_ERR_NO_TMD_FILE_FOUND || utility::ESMisc::checkContentsNum(titleId, tmd)) {
                if (ret != ES_ERR_OK && ret != ES_ERR_NO_TMD_FILE_FOUND) {
                    IPLErrorLogAndDisplay(MESG_ERR_FILE, "ES", ret, 1149);
                }
                result = FALSE;
            }

            if (tmd) {
                mpHeap->free(tmd);
            }

            return result;
        }

        int Manager::getAvailableNumInList(const ESTitleId* titleIds, u32 titleCount) {
            int count = 0;
            for (int i = 0; i < titleCount; i++) {
                if (titleIds[i] == 0) {
                    count++;
                }
            }
            return count - 1;
        }

        int Manager::getAvailableInList(const ESTitleId* titleIds, u32 titleCount) {
            int channelsPerPage = MAX_CHANNEL_INDEX;
            u32 titleIndex;
            for (titleIndex = 0; titleIndex < titleCount; titleIndex++) {
                if (titleIds[titleIndex] == TITLE_NULL) {
                    int page = (int)titleIndex / channelsPerPage;
                    int index = (int)titleIndex % channelsPerPage;
                    // MWCC needs this address order to preserve the original channel-slot loads.
                    u8* channelPage = (u8*)this + page * sizeof(mData.chanInfo[page]);
                    channelPage += index * sizeof(channel::SInfo);
                    if (*(channelPage + 0x30) != channel::PRIMARY_TYPE_DISK) {
                        return (int)titleIndex;
                    }
                }
            }
            return -1;
        }

        int Manager::isEqualChannel(ESTitleId titleId0, ESTitleId titleId1) {
            if (titleId0 == titleId1) {
                return -2;
            }

            if (TITLE_NO_REGION(titleId0) == TITLE_NO_REGION(titleId1)) {
                if (TITLE_REGION(titleId0) != TITLE_REGION_ALL) {
                    if (TITLE_REGION(titleId1) == TITLE_REGION_ALL) {
                        return 0;
                    }
                }

                if (TITLE_REGION(titleId0) == TITLE_REGION_ALL) {
                    if (TITLE_REGION(titleId1) != TITLE_REGION_ALL) {
                        return 1;
                    }
                }

                return -1;
            }

            return -1;
        }

        int Manager::isDefaultChannel(ESTitleId32 titleType, ESTitleId32 titleCode) {
            for (int i = 0; i < DEFAULT_CHANNEL_COUNT; i++) {
                if (cDefaultChanList[i].titleType == titleType && (cDefaultChanList[i].titleCode & 0xFFFFFF00) == (titleCode & 0xFFFFFF00)) {
                    return i;
                }
            }
            return -1;
        }

        void Manager::updateVersion(u32 newVersion, u32 oldVersion) {
            if ((newVersion & 0xFFFF0000) != (oldVersion & 0xFFFF0000)) {
                OSReport("[SaveDataManager] Invalid major version change %d -> %d\n", oldVersion >> 16, newVersion >> 16);
                OSHalt("\0\0\0", 1344);
            }
            mData.fileSize = sizeof(Data);
            mData.version = newVersion;

            switch (newVersion) {
                case 2: {
                    setDefaultKeyboard();
                    break;
                }
                case 3: {
                    switch ((u16)oldVersion) {
                        case 0:
                        case 1: {
                            setDefaultKeyboard();
                        }
                        case 2: {
                            setDefaultSDMenu();
                        }
                        default: {
                            break;
                        }
                    }
                    break;
                }
            }
        }

        void Manager::setDefaultSDMenu() {
            mData.didntGotoSDMenu = TRUE;
            memset(mData.titleCache, 0, sizeof(mData.titleCache));
            mData.prevSDPage = 0;
        }

#if defined(__MWERKS__) || defined(RVL_WEB)
        void Manager::pushTitleCache(ESTitleId titleId) {
            if (titleId == ES_TITLE_ID(TITLE_TYPE_CHANNEL, 'HATE') || titleId == ES_TITLE_ID(TITLE_TYPE_CHANNEL, 'HADE')) {
                return;
            }
            u32 titleType = ES_TITLE_TYPE_NOMASK(titleId);
            if (titleType != TITLE_TYPE_CHANNEL && titleType != TITLE_TYPE_UNK3 &&
                titleType != TITLE_TYPE_DISC_CHANNEL && titleType != TITLE_TYPE_UNK6) {
                return;
            }
            int index = 0;
            for (; index < MAX_CHANNEL_TOTAL; index++) {
                if ((&mData.titleCache[0][0])[index] == titleId) {
                    break;
                }
            }
            if (index == MAX_CHANNEL_TOTAL) {
                index = MAX_CHANNEL_TOTAL - 1;
            }
            for (; index > 0; index--) {
                u32 previousIndex = index - 1;
                (&mData.titleCache[0][0])[index] = (&mData.titleCache[0][0])[previousIndex];
            }
            mData.titleCache[0][0] = titleId;
        }

        extern "C" BOOL isTitleCached(void* manager, ESTitleId titleId) {
            Manager* savedataManager = static_cast<Manager*>(manager);
            for (int index = 0; index < MAX_CHANNEL_TOTAL; index++) {
                if (savedataManager->mData.titleCache[0][index] == titleId) {
                    return TRUE;
                }
            }
            return FALSE;
        }
#endif

        BOOL Manager::nand_error_handling(int code) {
            BOOL result = FALSE;

            mLastError = code;
            if (code >= NAND_RESULT_OK) {
                return TRUE;
            } else {
                switch (code) {
                    case NAND_RESULT_AUTHENTICATION:
                    case NAND_RESULT_NOEXISTS:
                    case NAND_RESULT_ECC_CRIT: {
                        break;
                    }
                    case NAND_RESULT_EXISTS: {
                        result = TRUE;
                        break;
                    }
                    case NAND_RESULT_CORRUPT: {
                        IPLErrorDisplay(MESG_ERR_NAND);
                        break;
                    }
                    case NAND_RESULT_OPENFD:
                    case NAND_RESULT_NOTEMPTY:
                    case NAND_RESULT_MAXFILES:
                    case NAND_RESULT_MAXFD:
                    case NAND_RESULT_MAXBLOCKS:
                    case NAND_RESULT_INVALID:
                    case NAND_RESULT_UNUSED_7:
                    default: {
                        IPLErrorLogAndDisplay(MESG_ERR_FILE, "NAND", code, 1503);
                        break;
                    }
                }
            }

            return result;
        }
    }  // namespace savedata
}  // namespace ipl
