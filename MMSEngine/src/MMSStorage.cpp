
#include "MMSStorage.h"
#include "catralibraries/FileIO.h"
#include "catralibraries/System.h"
#include "catralibraries/DateTime.h"

MMSStorage::MMSStorage(
        Json::Value configuration,
        shared_ptr<MMSEngineDBFacade> mmsEngineDBFacade,
        shared_ptr<spdlog::logger> logger) 
{

    _logger             = logger;
    _mmsEngineDBFacade  = mmsEngineDBFacade;

    _hostName = System::getHostName();

    _storage = configuration["storage"].get("path", "XXX").asString();
    if (_storage.back() != '/')
        _storage.push_back('/');
    _freeSpaceToLeaveInEachPartitionInMB = configuration["storage"].get("freeSpaceToLeaveInEachPartitionInMB", 5).asInt();

    _ingestionRootRepository = _storage + "IngestionRepository/users/";
    _mmsRootRepository = _storage + "MMSRepository/";
    _downloadRootRepository = _storage + "DownloadRepository/";
    _streamingRootRepository = _storage + "StreamingRepository/";

    _stagingRootRepository = _storage + "MMSWorkingAreaRepository/Staging/";

    _profilesRootRepository = _storage + "MMSRepository/EncodingProfiles/";

    bool noErrorIfExists = true;
    bool recursive = true;
    _logger->info(__FILEREF__ + "Creating directory (if needed)"
        + ", _ingestionRootRepository: " + _ingestionRootRepository
    );
    FileIO::createDirectory(_ingestionRootRepository,
            S_IRUSR | S_IWUSR | S_IXUSR |
            S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH, noErrorIfExists, recursive);

    _logger->info(__FILEREF__ + "Creating directory (if needed)"
        + ", _mmsRootRepository: " + _mmsRootRepository
    );
    FileIO::createDirectory(_mmsRootRepository,
            S_IRUSR | S_IWUSR | S_IXUSR |
            S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH, noErrorIfExists, recursive);

    // create MMS_0000 in case it does not exist (first running of MMS)
    {
        string MMS_0000Path = _mmsRootRepository + "MMS_0000";


        _logger->info(__FILEREF__ + "Creating directory (if needed)"
            + ", MMS_0000 Path: " + MMS_0000Path
        );
        FileIO::createDirectory(MMS_0000Path,
                S_IRUSR | S_IWUSR | S_IXUSR |
                S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH, noErrorIfExists, recursive);
    }

    _logger->info(__FILEREF__ + "Creating directory (if needed)"
        + ", _downloadRootRepository: " + _downloadRootRepository
    );
    FileIO::createDirectory(_downloadRootRepository,
            S_IRUSR | S_IWUSR | S_IXUSR |
            S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH, noErrorIfExists, recursive);

    _logger->info(__FILEREF__ + "Creating directory (if needed)"
        + ", _streamingRootRepository: " + _streamingRootRepository
    );
    FileIO::createDirectory(_streamingRootRepository,
            S_IRUSR | S_IWUSR | S_IXUSR |
            S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH, noErrorIfExists, recursive);

    _logger->info(__FILEREF__ + "Creating directory (if needed)"
        + ", _profilesRootRepository: " + _profilesRootRepository
    );
    FileIO::createDirectory(_profilesRootRepository,
            S_IRUSR | S_IWUSR | S_IXUSR |
            S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH, noErrorIfExists, recursive);

    _logger->info(__FILEREF__ + "Creating directory (if needed)"
        + ", _stagingRootRepository: " + _stagingRootRepository
    );
    FileIO::createDirectory(_stagingRootRepository,
            S_IRUSR | S_IWUSR | S_IXUSR |
            S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH, noErrorIfExists, recursive);

    // Partitions staff
    {
        char pMMSPartitionName [64];
        unsigned long long ullUsedInKB;
        unsigned long long ullAvailableInKB;
        long lPercentUsed;


        lock_guard<recursive_mutex> locker(_mtMMSPartitions);

        unsigned long ulMMSPartitionsNumber = 0;
        bool mmsAvailablePartitions = true;

        _ulCurrentMMSPartitionIndex = 0;

        // inizializzare FreeSize
        while (mmsAvailablePartitions) 
        {
            string pathNameToGetFileSystemInfo(_mmsRootRepository);

            sprintf(pMMSPartitionName, "MMS_%04lu", ulMMSPartitionsNumber);

            pathNameToGetFileSystemInfo.append(pMMSPartitionName);

            try 
            {
                FileIO::getFileSystemInfo(pathNameToGetFileSystemInfo,
                        &ullUsedInKB, &ullAvailableInKB, &lPercentUsed);
            }            
            catch (...) 
            {
                break;
            }

            _mmsPartitionsFreeSizeInMB.push_back(ullAvailableInKB / 1024);

            ulMMSPartitionsNumber++;
        }

        if (ulMMSPartitionsNumber == 0) {
            throw runtime_error("No MMS partition found");
        }

        refreshPartitionsFreeSizes();
    }
}

MMSStorage::~MMSStorage(void) {
}

string MMSStorage::getMMSRootRepository(void) {
    return _mmsRootRepository;
}

string MMSStorage::getStreamingRootRepository(void) {
    return _streamingRootRepository;
}

string MMSStorage::getDownloadRootRepository(void) {
    return _downloadRootRepository;
}

string MMSStorage::getIngestionRootRepository(void) {
    return _ingestionRootRepository;
}

string MMSStorage::getPhysicalPath(int64_t mediaItemKey,
        int64_t encodingProfileKey)
{    
    tuple<int,string,string,string> storageDetails =
        _mmsEngineDBFacade->getStorageDetails(mediaItemKey, encodingProfileKey);

    int mmsPartitionNumber;
    string customerDirectoryName;
    string relativePath;
    string fileName;
    tie(mmsPartitionNumber, customerDirectoryName, relativePath, fileName) = storageDetails;

    return getMMSAssetPathName(
        mmsPartitionNumber,
        customerDirectoryName,
        relativePath,
        fileName);
}

string MMSStorage::getCustomerIngestionRepository(shared_ptr<Customer> customer)
{
    string customerIngestionDirectory = getIngestionRootRepository();
    customerIngestionDirectory.append(customer->_name);
    
    if (!FileIO::directoryExisting(customerIngestionDirectory)) 
    {
        _logger->info(__FILEREF__ + "Create directory"
            + ", customerIngestionDirectory: " + customerIngestionDirectory
        );

        bool noErrorIfExists = true;
        bool recursive = true;
        FileIO::createDirectory(customerIngestionDirectory,
                S_IRUSR | S_IWUSR | S_IXUSR |
                S_IRGRP | S_IXGRP |
                S_IROTH | S_IXOTH, noErrorIfExists, recursive);
    }

    return customerIngestionDirectory;
}

string MMSStorage::getStagingRootRepository(void) {
    return _stagingRootRepository;
}

void MMSStorage::moveContentInRepository(
        string filePathName,
        RepositoryType rtRepositoryType,
        string customerDirectoryName,
        bool addDateTimeToFileName)
 {

    contentInRepository(
        1,
        filePathName,
        rtRepositoryType,
        customerDirectoryName,
        addDateTimeToFileName);
}

void MMSStorage::copyFileInRepository(
        string filePathName,
        RepositoryType rtRepositoryType,
        string customerDirectoryName,
        bool addDateTimeToFileName)
 {

    contentInRepository(
        0,
        filePathName,
        rtRepositoryType,
        customerDirectoryName,
        addDateTimeToFileName);
}

string MMSStorage::getRepository(RepositoryType rtRepositoryType) 
{

    switch (rtRepositoryType) 
    {
        case RepositoryType::MMSREP_REPOSITORYTYPE_MMSCUSTOMER:
        {
            return _mmsRootRepository;
        }
        case RepositoryType::MMSREP_REPOSITORYTYPE_DOWNLOAD:
        {
            return _downloadRootRepository;
        }
        case RepositoryType::MMSREP_REPOSITORYTYPE_STREAMING:
        {
            return _streamingRootRepository;
        }
        case RepositoryType::MMSREP_REPOSITORYTYPE_STAGING:
        {
            return _stagingRootRepository;
        }
        case RepositoryType::MMSREP_REPOSITORYTYPE_INGESTION:
        {
            return _ingestionRootRepository;
        }
        default:
        {
            throw runtime_error(string("Wrong argument")
                    + ", rtRepositoryType: " + to_string(static_cast<int>(rtRepositoryType))
                    );
        }
    }
}

void MMSStorage::contentInRepository(
        unsigned long ulIsCopyOrMove,
        string contentPathName,
        RepositoryType rtRepositoryType,
        string customerDirectoryName,
        bool addDateTimeToFileName)
 {

    tm tmDateTime;
    unsigned long ulMilliSecs;
    FileIO::DirectoryEntryType_t detSourceFileType;


    // pDestRepository includes the '/' at the end
    string metaDataFileInDestRepository(getRepository(rtRepositoryType));
    metaDataFileInDestRepository
        .append(customerDirectoryName)
        .append("/");

    DateTime::get_tm_LocalTime(&tmDateTime, &ulMilliSecs);

    if (rtRepositoryType == RepositoryType::MMSREP_REPOSITORYTYPE_STAGING) 
    {
        char pDateTime [64];
        bool directoryExisting;


        sprintf(pDateTime,
                "%04lu_%02lu_%02lu",
                (unsigned long) (tmDateTime. tm_year + 1900),
                (unsigned long) (tmDateTime. tm_mon + 1),
                (unsigned long) (tmDateTime. tm_mday));

        metaDataFileInDestRepository.append(pDateTime);

        if (!FileIO::directoryExisting(metaDataFileInDestRepository)) 
        {
            _logger->info(__FILEREF__ + "Create directory"
                + ", metaDataFileInDestRepository: " + metaDataFileInDestRepository
            );

            bool noErrorIfExists = true;
            bool recursive = true;
            FileIO::createDirectory(metaDataFileInDestRepository,
                    S_IRUSR | S_IWUSR | S_IXUSR |
                    S_IRGRP | S_IXGRP |
                    S_IROTH | S_IXOTH, noErrorIfExists, recursive);
        }

        metaDataFileInDestRepository.append("/");
    }

    if (addDateTimeToFileName) 
    {
        char pDateTime [64];


        sprintf(pDateTime,
                "%04lu_%02lu_%02lu_%02lu_%02lu_%02lu_%04lu_",
                (unsigned long) (tmDateTime. tm_year + 1900),
                (unsigned long) (tmDateTime. tm_mon + 1),
                (unsigned long) (tmDateTime. tm_mday),
                (unsigned long) (tmDateTime. tm_hour),
                (unsigned long) (tmDateTime. tm_min),
                (unsigned long) (tmDateTime. tm_sec),
                ulMilliSecs);

        metaDataFileInDestRepository.append(pDateTime);
    }

    size_t fileNameStart;
    string fileName;
    if ((fileNameStart = contentPathName.find_last_of('/')) == string::npos)
        fileName = contentPathName;
    else
        fileName = contentPathName.substr(fileNameStart + 1);

    metaDataFileInDestRepository.append(fileName);

    // file in case of .3gp content OR
    // directory in case of IPhone content
    detSourceFileType = FileIO::getDirectoryEntryType(contentPathName);

    if (ulIsCopyOrMove == 1) 
    {
        if (detSourceFileType == FileIO::TOOLS_FILEIO_DIRECTORY) 
        {
            _logger->info(__FILEREF__ + "Move directory"
                + ", from: " + contentPathName
                + ", to: " + metaDataFileInDestRepository
            );

            FileIO::moveDirectory(contentPathName,
                    metaDataFileInDestRepository,
                    S_IRUSR | S_IWUSR | S_IXUSR |
                    S_IRGRP | S_IXGRP |
                    S_IROTH | S_IXOTH);
        } 
        else // if (detSourceFileType == FileIO:: TOOLS_FILEIO_REGULARFILE
        {
            _logger->info(__FILEREF__ + "Move file"
                + ", from: " + contentPathName
                + ", to: " + metaDataFileInDestRepository
            );

            FileIO::moveFile(contentPathName, metaDataFileInDestRepository);
        }
    } 
    else 
    {
        if (detSourceFileType == FileIO::TOOLS_FILEIO_DIRECTORY) 
        {
            _logger->info(__FILEREF__ + "Copy directory"
                + ", from: " + contentPathName
                + ", to: " + metaDataFileInDestRepository
            );

            FileIO::copyDirectory(contentPathName,
                    metaDataFileInDestRepository,
                    S_IRUSR | S_IWUSR | S_IXUSR |
                    S_IRGRP | S_IXGRP |
                    S_IROTH | S_IXOTH);
        } 
        else 
        {
            _logger->info(__FILEREF__ + "Copy file"
                + ", from: " + contentPathName
                + ", to: " + metaDataFileInDestRepository
            );

            FileIO::copyFile(contentPathName,
                    metaDataFileInDestRepository);
        }
    }
}

string MMSStorage::moveAssetInMMSRepository(
        string sourceAssetPathName,
        string customerDirectoryName,
        string destinationAssetFileName,
        string relativePath,

        bool partitionIndexToBeCalculated,
        unsigned long *pulMMSPartitionIndexUsed, // OUT if bIsPartitionIndexToBeCalculated is true, IN is bIsPartitionIndexToBeCalculated is false

        bool deliveryRepositoriesToo,
        Customer::TerritoriesHashMap& phmTerritories
        )
 {
    FileIO::DirectoryEntryType_t detSourceFileType;


    if (relativePath.front() != '/' || pulMMSPartitionIndexUsed == (unsigned long *) NULL) 
    {
            throw runtime_error(string("Wrong argument")
                    + ", relativePath: " + relativePath
                    );
    }

    lock_guard<recursive_mutex> locker(_mtMMSPartitions);

    // file in case of .3gp content OR
    // directory in case of IPhone content
    detSourceFileType = FileIO::getDirectoryEntryType(sourceAssetPathName);

    if (detSourceFileType != FileIO::TOOLS_FILEIO_DIRECTORY &&
            detSourceFileType != FileIO::TOOLS_FILEIO_REGULARFILE) 
    {
        throw runtime_error("Wrong directory entry type");
    }

    if (partitionIndexToBeCalculated) 
    {
        unsigned long long ullFSEntrySizeInBytes;


        if (detSourceFileType == FileIO::TOOLS_FILEIO_DIRECTORY) 
        {
            ullFSEntrySizeInBytes = FileIO::getDirectorySizeInBytes(sourceAssetPathName);
        } 
        else // if (detSourceFileType == FileIO:: TOOLS_FILEIO_REGULARFILE)
        {
            unsigned long ulFileSizeInBytes;
            bool inCaseOfLinkHasItToBeRead = false;


            ulFileSizeInBytes = FileIO::getFileSizeInBytes(sourceAssetPathName, inCaseOfLinkHasItToBeRead);

            ullFSEntrySizeInBytes = ulFileSizeInBytes;
        }

        // find the MMS partition index
        unsigned long ulMMSPartitionIndex;
        for (ulMMSPartitionIndex = 0;
                ulMMSPartitionIndex < _mmsPartitionsFreeSizeInMB.size();
                ulMMSPartitionIndex++) 
        {
            unsigned long long mmsPartitionsFreeSizeInKB = (unsigned long long)
                ((_mmsPartitionsFreeSizeInMB [_ulCurrentMMSPartitionIndex]) * 1024);

            if (mmsPartitionsFreeSizeInKB <=
                    (_freeSpaceToLeaveInEachPartitionInMB * 1024)) 
            {
                _logger->info(__FILEREF__ + "Partition space too low"
                    + ", _ulCurrentMMSPartitionIndex: " + to_string(_ulCurrentMMSPartitionIndex)
                    + ", mmsPartitionsFreeSizeInKB: " + to_string(mmsPartitionsFreeSizeInKB)
                    + ", _freeSpaceToLeaveInEachPartitionInMB * 1024: " + to_string(_freeSpaceToLeaveInEachPartitionInMB * 1024)
                );

                if (_ulCurrentMMSPartitionIndex + 1 >= _mmsPartitionsFreeSizeInMB.size())
                    _ulCurrentMMSPartitionIndex = 0;
                else
                    _ulCurrentMMSPartitionIndex++;

                continue;
            }

            if ((unsigned long long) (mmsPartitionsFreeSizeInKB -
                    (_freeSpaceToLeaveInEachPartitionInMB * 1024)) >
                    (ullFSEntrySizeInBytes / 1024)) 
            {
                break;
            }

            if (_ulCurrentMMSPartitionIndex + 1 >= _mmsPartitionsFreeSizeInMB.size())
                _ulCurrentMMSPartitionIndex = 0;
            else
                _ulCurrentMMSPartitionIndex++;
        }

        if (ulMMSPartitionIndex == _mmsPartitionsFreeSizeInMB.size()) 
        {
            throw runtime_error(string("No more space in MMS Partitions")
                    + ", ullFSEntrySizeInBytes: " + to_string(ullFSEntrySizeInBytes)
                    );
        }

        *pulMMSPartitionIndexUsed = _ulCurrentMMSPartitionIndex;
    }

    // creating directories and build the bMMSAssetPathName
    string mmsAssetPathName;
    {
        // to create the content provider directory and the
        // territories directories (if not already existing)
        mmsAssetPathName = creatingDirsUsingTerritories(*pulMMSPartitionIndexUsed,
            relativePath, customerDirectoryName, deliveryRepositoriesToo,
            phmTerritories);

        mmsAssetPathName.append(destinationAssetFileName);
    }

    _logger->info(__FILEREF__ + "Selected MMS Partition for the content"
        + ", customerDirectoryName: " + customerDirectoryName
        + ", *pulMMSPartitionIndexUsed: " + to_string(*pulMMSPartitionIndexUsed)
        + ", mmsAssetPathName: " + mmsAssetPathName
        + ", _mmsPartitionsFreeSizeInMB [_ulCurrentMMSPartitionIndex]: " + to_string(_mmsPartitionsFreeSizeInMB [_ulCurrentMMSPartitionIndex])
    );

    // move the file in case of .3gp content OR
    // move the directory in case of IPhone content
    {
        if (detSourceFileType == FileIO::TOOLS_FILEIO_DIRECTORY) 
        {
            _logger->info(__FILEREF__ + "Move directory"
                + ", from: " + sourceAssetPathName
                + ", to: " + mmsAssetPathName
            );

            FileIO::moveDirectory(sourceAssetPathName,
                    mmsAssetPathName,
                    S_IRUSR | S_IWUSR | S_IXUSR |
                    S_IRGRP | S_IXGRP |
                    S_IROTH | S_IXOTH);
        } 
        else // if (detDirectoryEntryType == FileIO:: TOOLS_FILEIO_REGULARFILE)
        {
            _logger->info(__FILEREF__ + "Move file"
                + ", from: " + sourceAssetPathName
                + ", to: " + mmsAssetPathName
            );

            FileIO::moveFile(sourceAssetPathName,
                    mmsAssetPathName);
        }
    }

    // update _pullMMSPartitionsFreeSizeInMB ONLY if bIsPartitionIndexToBeCalculated
    if (partitionIndexToBeCalculated) 
    {
        unsigned long long ullUsedInKB;
        unsigned long long ullAvailableInKB;
        long lPercentUsed;


        FileIO::getFileSystemInfo(mmsAssetPathName,
                &ullUsedInKB, &ullAvailableInKB, &lPercentUsed);

        _mmsPartitionsFreeSizeInMB [_ulCurrentMMSPartitionIndex] =
            ullAvailableInKB / 1024;

        _logger->info(__FILEREF__ + "Available space"
            + ", mmsAssetPathName: " + mmsAssetPathName
            + ", _mmsPartitionsFreeSizeInMB[_ulCurrentMMSPartitionIndex]: " + to_string(_mmsPartitionsFreeSizeInMB[_ulCurrentMMSPartitionIndex])
        );
    }


    return mmsAssetPathName;
}

string MMSStorage::getMMSAssetPathName(
        unsigned long ulPartitionNumber,
        string customerDirectoryName,
        string relativePath, // using '/'
        string fileName)
 {
    char pMMSPartitionName [64];


    sprintf(pMMSPartitionName, "MMS_%04lu/", ulPartitionNumber);

    string assetPathName(_mmsRootRepository);
    assetPathName
        .append(pMMSPartitionName)
        .append(customerDirectoryName)
        .append(relativePath)
        .append(fileName);


    return assetPathName;
}

string MMSStorage::getDownloadLinkPathName(
        unsigned long ulPartitionNumber,
        string customerDirectoryName,
        string territoryName,
        string relativePath,
        string fileName,
        bool downloadRepositoryToo)
 {

    char pMMSPartitionName [64];
    string linkPathName;

    if (downloadRepositoryToo) 
    {
        sprintf(pMMSPartitionName, "MMS_%04lu/", ulPartitionNumber);

        linkPathName = _downloadRootRepository;
        linkPathName
            .append(pMMSPartitionName)
            .append(customerDirectoryName)
            .append("/")
            .append(territoryName)
            .append(relativePath)
            .append(fileName);
    } 
    else
    {
        sprintf(pMMSPartitionName, "/MMS_%04lu/", ulPartitionNumber);

        linkPathName = pMMSPartitionName;
        linkPathName
            .append(customerDirectoryName)
            .append("/")
            .append(territoryName)
            .append(relativePath)
            .append(fileName);
    }


    return linkPathName;
}

string MMSStorage::getStreamingLinkPathName(
        unsigned long ulPartitionNumber, // IN
        string customerDirectoryName, // IN
        string territoryName, // IN
        string relativePath, // IN
        string fileName) // IN
 {
    char pMMSPartitionName [64];
    string linkPathName;


    sprintf(pMMSPartitionName, "MMS_%04lu/", ulPartitionNumber);

    linkPathName = _streamingRootRepository;
    linkPathName
        .append(pMMSPartitionName)
        .append(customerDirectoryName)
        .append("/")
        .append(territoryName)
        .append(relativePath)
        .append(fileName);


    return linkPathName;
}

string MMSStorage::getStagingAssetPathName(
        string customerDirectoryName,
        string relativePath,
        string fileName,                // may be empty ("")
        long long llMediaItemKey,       // used only if fileName is ""
        long long llPhysicalPathKey,    // used only if fileName is ""
        bool removeLinuxPathIfExist)
 {
    char pUniqueFileName [256];
    string localFileName;
    tm tmDateTime;
    unsigned long ulMilliSecs;
    char pDateTime [64];
    string assetPathName;


    DateTime::get_tm_LocalTime(&tmDateTime, &ulMilliSecs);

    if (fileName == "") 
    {
        sprintf(pUniqueFileName,
                "%04lu_%02lu_%02lu_%02lu_%02lu_%02lu_%04lu_%lld_%lld_%s",
                (unsigned long) (tmDateTime. tm_year + 1900),
                (unsigned long) (tmDateTime. tm_mon + 1),
                (unsigned long) (tmDateTime. tm_mday),
                (unsigned long) (tmDateTime. tm_hour),
                (unsigned long) (tmDateTime. tm_min),
                (unsigned long) (tmDateTime. tm_sec),
                ulMilliSecs,
                llMediaItemKey,
                llPhysicalPathKey,
                _hostName.c_str());

        localFileName = pUniqueFileName;
    } 
    else 
    {
        localFileName = fileName;
    }

    sprintf(pDateTime,
            "%04lu_%02lu_%02lu",
            (unsigned long) (tmDateTime. tm_year + 1900),
            (unsigned long) (tmDateTime. tm_mon + 1),
            (unsigned long) (tmDateTime. tm_mday));

    // create the 'date' directory in staging if not exist
    {
        assetPathName = _stagingRootRepository;
        assetPathName
            .append(customerDirectoryName)
            .append("/")
            .append(pDateTime)
            .append(relativePath);

        if (!FileIO::directoryExisting(assetPathName)) 
        {
            _logger->info(__FILEREF__ + "Create directory"
                + ", assetPathName: " + assetPathName
            );

            bool noErrorIfExists = true;
            bool recursive = true;
            FileIO::createDirectory(
                    assetPathName,
                    S_IRUSR | S_IWUSR | S_IXUSR |
                    S_IRGRP | S_IXGRP |
                    S_IROTH | S_IXOTH, noErrorIfExists, recursive);
        }
    }

    {
        assetPathName.append(localFileName);

        if (removeLinuxPathIfExist) 
        {
            FileIO::DirectoryEntryType_t detSourceFileType;

            try 
            {
                detSourceFileType = FileIO::getDirectoryEntryType(assetPathName);

                if (detSourceFileType == FileIO::TOOLS_FILEIO_DIRECTORY) 
                {
                    _logger->info(__FILEREF__ + "Remove directory"
                        + ", assetPathName: " + assetPathName
                    );

                    bool removeRecursively = true;
                    FileIO::removeDirectory(assetPathName, removeRecursively);
                } 
                else if (detSourceFileType == FileIO::TOOLS_FILEIO_REGULARFILE) 
                {
                    _logger->info(__FILEREF__ + "Remove file"
                        + ", assetPathName: " + assetPathName
                    );

                    FileIO::remove(assetPathName);
                } 
                else 
                {
                    throw runtime_error(string("Unexpected file in staging")
                            + ", assetPathName: " + assetPathName
                            );
                }
            }
            catch (...) 
            {
                //				 * the entry does not exist
                //				 *
                //				_ptSystemTracer -> trace (Tracer:: TRACER_LERRR,
                //					(const char *) errFileIO, __FILE__, __LINE__);
                //
                //				Error err = ToolsErrors (__FILE__, __LINE__,
                //					TOOLS_FILEIO_GETDIRECTORYENTRYTYPE_FAILED,
                //					1, (const char *) bContentPathName);
                //				_ptSystemTracer -> trace (Tracer:: TRACER_LERRR,
                //					(const char *) err, __FILE__, __LINE__);
                //
                //				return err;
            }
        }
    }


    return assetPathName;
}

string MMSStorage::getEncodingProfilePathName(
        long long llEncodingProfileKey,
        string profileFileNameExtension)
 {
    string encodingProfilePathName(_profilesRootRepository);

    encodingProfilePathName
        .append(to_string(llEncodingProfileKey))
        .append(profileFileNameExtension);

    return encodingProfilePathName;
}

string MMSStorage::getFFMPEGEncodingProfilePathName(
        MMSEngineDBFacade::ContentType contentType,
        long long llEncodingProfileKey)
 {

    if (contentType != MMSEngineDBFacade::ContentType::Video && 
            contentType != MMSEngineDBFacade::ContentType::Audio &&
            contentType != MMSEngineDBFacade::ContentType::Image)
    {
        throw runtime_error(string("Wrong argument")
                + ", contentType: " + to_string(static_cast<int>(contentType))
                );
    }

    string encodingProfilePathName(_profilesRootRepository);

    encodingProfilePathName
        .append(to_string(llEncodingProfileKey));

    if (contentType == MMSEngineDBFacade::ContentType::Video)
    {
        encodingProfilePathName.append(".vep");
    } 
    else if (contentType == MMSEngineDBFacade::ContentType::Audio)
    {
        encodingProfilePathName.append(".aep");
    } 
    else if (contentType == MMSEngineDBFacade::ContentType::Image)
    {
        encodingProfilePathName.append(".iep");
    }


    return encodingProfilePathName;
}

unsigned long MMSStorage::getCustomerStorageUsage(
        string customerDirectoryName)
 {

    unsigned long ulStorageUsageInMB;

    unsigned long ulMMSPartitionIndex;
    unsigned long long ullDirectoryUsageInBytes;
    unsigned long long ullCustomerStorageUsageInBytes;


    lock_guard<recursive_mutex> locker(_mtMMSPartitions);

    ullCustomerStorageUsageInBytes = 0;

    for (ulMMSPartitionIndex = 0;
            ulMMSPartitionIndex < _mmsPartitionsFreeSizeInMB.size();
            ulMMSPartitionIndex++) 
    {
        string contentProviderPathName = getMMSAssetPathName(
                ulMMSPartitionIndex, customerDirectoryName,
                string(""), string(""));

        try 
        {
            ullDirectoryUsageInBytes = FileIO::getDirectoryUsage(contentProviderPathName);
        } 
        catch (DirectoryNotExisting d) 
        {
            continue;
        } 
        catch (...) 
        {
            throw runtime_error(string("FileIO:: getDirectoryUsage failed")
                    + ", contentProviderPathName: " + contentProviderPathName
                    );
        }

        ullCustomerStorageUsageInBytes += ullDirectoryUsageInBytes;
    }


    ulStorageUsageInMB = (unsigned long)
            (ullCustomerStorageUsageInBytes / (1024 * 1024));

    return ulStorageUsageInMB;
}

void MMSStorage::refreshPartitionsFreeSizes(void) 
{
    char pMMSPartitionName [64];
    unsigned long long ullUsedInKB;
    unsigned long long ullAvailableInKB;
    long lPercentUsed;


    lock_guard<recursive_mutex> locker(_mtMMSPartitions);

    for (unsigned long ulMMSPartitionIndex = 0;
            ulMMSPartitionIndex < _mmsPartitionsFreeSizeInMB.size();
            ulMMSPartitionIndex++) 
    {
        string pathNameToGetFileSystemInfo(_mmsRootRepository);

        sprintf(pMMSPartitionName, "MMS_%04lu", ulMMSPartitionIndex);

        pathNameToGetFileSystemInfo.append(pMMSPartitionName);

        FileIO::getFileSystemInfo(pathNameToGetFileSystemInfo,
                &ullUsedInKB, &ullAvailableInKB, &lPercentUsed);

        _mmsPartitionsFreeSizeInMB[ulMMSPartitionIndex] =
                ullAvailableInKB / 1024;

        _logger->info(__FILEREF__ + "Available space"
            + ", pathNameToGetFileSystemInfo: " + pathNameToGetFileSystemInfo
            + ", _mmsPartitionsFreeSizeInMB[ulMMSPartitionIndex]: " + to_string(_mmsPartitionsFreeSizeInMB[ulMMSPartitionIndex])
        );
    }
}

string MMSStorage::creatingDirsUsingTerritories(
        unsigned long ulCurrentMMSPartitionIndex,
        string relativePath,
        string customerDirectoryName,
        bool deliveryRepositoriesToo,
        Customer::TerritoriesHashMap& phmTerritories)
 {

    char pMMSPartitionName [64];


    sprintf(pMMSPartitionName, "MMS_%04lu/", ulCurrentMMSPartitionIndex);

    string mmsAssetPathName(_mmsRootRepository);
    mmsAssetPathName
        .append(pMMSPartitionName)
        .append(customerDirectoryName)
        .append(relativePath);

    if (!FileIO::directoryExisting(mmsAssetPathName)) 
    {
        _logger->info(__FILEREF__ + "Create directory"
            + ", mmsAssetPathName: " + mmsAssetPathName
        );

        bool noErrorIfExists = true;
        bool recursive = true;
        FileIO::createDirectory(mmsAssetPathName,
                S_IRUSR | S_IWUSR | S_IXUSR |
                S_IRGRP | S_IXGRP |
                S_IROTH | S_IXOTH, noErrorIfExists, recursive);
    }

    if (mmsAssetPathName.back() != '/')
        mmsAssetPathName.append("/");

    if (deliveryRepositoriesToo) 
    {
        Customer::TerritoriesHashMap::iterator it;


        for (it = phmTerritories.begin(); it != phmTerritories.end(); ++it) 
        {
            string territoryName = it->second;

            string downloadAssetPathName(_downloadRootRepository);
            downloadAssetPathName
                .append(pMMSPartitionName)
                .append(customerDirectoryName)
                .append("/")
                .append(territoryName)
                .append(relativePath);

            string streamingAssetPathName(_streamingRootRepository);
            streamingAssetPathName
                .append(pMMSPartitionName)
                .append(customerDirectoryName)
                .append("/")
                .append(territoryName)
                .append(relativePath);

            if (!FileIO::directoryExisting(downloadAssetPathName)) 
            {
                _logger->info(__FILEREF__ + "Create directory"
                    + ", downloadAssetPathName: " + downloadAssetPathName
                );
                
                bool noErrorIfExists = true;
                bool recursive = true;
                FileIO::createDirectory(downloadAssetPathName,
                        S_IRUSR | S_IWUSR | S_IXUSR |
                        S_IRGRP | S_IXGRP |
                        S_IROTH | S_IXOTH, noErrorIfExists, recursive);
            }

            if (!FileIO::directoryExisting(streamingAssetPathName)) 
            {
                _logger->info(__FILEREF__ + "Create directory"
                    + ", streamingAssetPathName: " + streamingAssetPathName
                );

                bool noErrorIfExists = true;
                bool recursive = true;
                FileIO::createDirectory(streamingAssetPathName,
                        S_IRUSR | S_IWUSR | S_IXUSR |
                        S_IRGRP | S_IXGRP |
                        S_IROTH | S_IXOTH, noErrorIfExists, recursive);
            }
        }
    }


    return mmsAssetPathName;
}

