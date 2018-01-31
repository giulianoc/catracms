
#include "CMSEngineProcessor.h"
#include "CheckIngestionTimes.h"

CMSEngineProcessor::CMSEngineProcessor(
        shared_ptr<spdlog::logger> logger, 
        shared_ptr<CMSEngineDBFacade> cmsEngineDBFacade,
        shared_ptr<CMSStorage> cmsStorage
)
{
    _logger             = logger;
    _cmsEngineDBFacade  = cmsEngineDBFacade;
    _cmsStorage      = cmsStorage;
    
    _ulIngestionLastCustomerIndex   = 0;
    
    _ulMaxIngestionsNumberPerCustomerEachIngestionPeriod       = 2;
    _ulJsonToBeProcessedAfterSeconds                           = 10;
}

CMSEngineProcessor::~CMSEngineProcessor()
{
    
}

void CMSEngineProcessor::operator ()(shared_ptr<MultiEventsSet> multiEventsSet) 
{
    bool blocking = true;
    chrono::milliseconds milliSecondsToBlock(100);

    // SPDLOG_DEBUG(_logger , "Enabled only #ifdef SPDLOG_TRACE_ON..{} ,{}", 1, 3.23);
    // SPDLOG_TRACE(_logger , "Enabled only #ifdef SPDLOG_TRACE_ON..{} ,{}", 1, 3.23);
    _logger->info("CMSEngineProcessor thread started");

    bool endEvent = false;
    while(!endEvent)
    {
        // cout << "Calling getAndRemoveFirstEvent" << endl;
        shared_ptr<Event> event = multiEventsSet->getAndRemoveFirstEvent(CMSENGINEPROCESSORNAME, blocking, milliSecondsToBlock);
        if (event == nullptr)
        {
            // cout << "No event found or event not yet expired" << endl;

            continue;
        }

        switch(event->getEventKey().first)
        {
            /*
            case GETDATAEVENT_TYPE:
            {                    
                shared_ptr<GetDataEvent>    getDataEvent = dynamic_pointer_cast<GetDataEvent>(event);
                cout << "getAndRemoveFirstEvent: GETDATAEVENT_TYPE (" << event->getEventKey().first << ", " << event->getEventKey().second << "): " << getDataEvent->getDataId() << endl << endl;
                multiEventsSet.getEventsFactory()->releaseEvent<GetDataEvent>(getDataEvent);
            }
            break;
            */
            case CMSENGINE_EVENTTYPEIDENTIFIER_CHECKINGESTION:	// 1
            {
                _logger->info("Received CMSENGINE_EVENTTYPEIDENTIFIER_CHECKINGESTION");

		handleCheckIngestionEvent ();

                multiEventsSet->getEventsFactory()->releaseEvent<Event>(event);

            }
            break;
            default:
                throw invalid_argument(string("Event type identifier not managed")
                        + to_string(event->getEventKey().first));
        }
    }

    _logger->info("CMSEngineProcessor thread terminated");
}

void CMSEngineProcessor::handleCheckIngestionEvent()
{
    /*
	CustomersHashMap_t:: iterator		itCustomersBegin;
	CustomersHashMap_t:: iterator		itCustomersEnd;
	CustomersHashMap_t:: iterator		itCustomers;
	Customer_p							pcCustomer;
	FileIO:: Directory_t				dDirectory;
	Error_t								errOpenDir;
	Error_t								errReadDirectory;
	Error_t								errMove;
	Error_t								errFileTime;
	FileIO:: DirectoryEntryType_t		detDirectoryEntryType;
	long long							llIngestionJobKey;
	unsigned long						ulCurrentCustomerIngestionsNumber;
	time_t								tLastModificationTime;
	unsigned long						ulCurrentCustomerIndex;
*/
    
    vector<shared_ptr<Customer>> customers = _cmsEngineDBFacade->getCustomers();
    
    unsigned long ulCurrentCustomerIndex = 0;
    vector<shared_ptr<Customer>>::iterator itCustomers;
    
    for (itCustomers = customers.begin();
            itCustomers != customers.end() && ulCurrentCustomerIndex < _ulIngestionLastCustomerIndex;
            ++itCustomers, ulCurrentCustomerIndex++)
    {
    }

    for (; itCustomers != customers.end(); ++itCustomers)
    {
        shared_ptr<Customer> customer = *itCustomers;

        try
        {
            _ulIngestionLastCustomerIndex++;

            string customerFTPDirectory = _cmsStorage->getCustomerFTPRepository(customer);

            shared_ptr<FileIO::Directory> directory = FileIO:: openDirectory(customerFTPDirectory);

            unsigned long ulCurrentCustomerIngestionsNumber		= 0;

            bool moreContentsToBeProcessed = true;
            while (moreContentsToBeProcessed)
            {
                FileIO:: DirectoryEntryType_t	detDirectoryEntryType;
                string directoryEntry;
                
                try
                {
                    directoryEntry = FileIO::readDirectory (directory,
                        &detDirectoryEntryType);
                }
                catch(DirectoryListFinished dlf)
                {
                    moreContentsToBeProcessed = false;

                    continue;
                }
                catch(...)
                {
                    _logger->error("FileIO::readDirectory failed");

                    moreContentsToBeProcessed = false;

                    break;
                }

                if (detDirectoryEntryType != FileIO:: TOOLS_FILEIO_REGULARFILE)
                {
                    continue;
                }

                // check if the file has ".json" as extension.
                // We do not accept also the ".json" file (without name)
                string jsonExtension(".json");
                if (directoryEntry.length() < 6 ||
                        !equal(jsonExtension.rbegin(), jsonExtension.rend(), directoryEntry.rbegin()))
                        continue;

                string srcPathName(customerFTPDirectory);
                srcPathName.append("/").append(directoryEntry);

                try
                {
                    tLastModificationTime = FileIO:: getFileTime (srcPathName);
                }
                catch(FileNotExisting fne)
                {
                    continue;
                }
                catch(runtime_error re)
                {
                    _logger->error(string("FileIO::getFileTime failed")
                        + ", srcPathName: " + srcPathName
                        + ", re.what: " + re.what()
                    );

                    continue;
                }
                
                if (time (NULL) - tLastModificationTime < _ulJsonToBeProcessedAfterSeconds)
                {
                    // only the json files having the last modification older
                    // at least of _ulJsonToBeProcessedAfterSeconds seconds
                    // are considered

                    continue;
                }

                _logger->info(string("json to be processed")
                    + ", directoryEntry: " + directoryEntry
                );

                string stagingAssetPathName = _cmsStorage->getStagingAssetPathName (
                    customer->_directoryName,
                    "/",            // relativePath
                    directoryEntry,
                    0, 0,           // llMediaItemKey, llPhysicalPathKey
                    true            // removeLinuxPathIfExist
                );

                _logger->info(string("Move file")
                    + ", from: " + srcPathName
                    + ", to: " + stagingAssetPathName
                );

                /*
                moveFile (srcPathName, stagingAssetPathName);

                if (IngestAssetThread:: insertIngestionJobStatusOnFileAndDB (
                        _pcrCMSRepository -> getFTPRootRepository (),
                        pcCustomer -> _bName. str(),
                        pcCustomer -> _bDirectoryName. str(),
                        _bDirectoryEntry. str(),
                        &llIngestionJobKey,
                        _plbWebServerLoadBalancer,
                        _pWebServerLocalIPAddress,
                        _ulWebServerTimeoutToWaitAnswerInSeconds,
                        _ptSystemTracer) != errNoError)
                {
                        Error err = CMSEngineErrors (__FILE__, __LINE__,
                CMS_INGESTASSETTHREAD_INSERTINGESTIONJOBSTATUSONFILEANDDB_FAILED);
                        _ptSystemTracer -> trace (Tracer:: TRACER_LERRR,
                                (const char *) err, __FILE__, __LINE__);

                        if (_pcrCMSRepository -> moveContentInRepository (
                                (const char *) _bDestPathName,
                                CMSRepository:: CMSREP_REPOSITORYTYPE_ERRORS,
                                (const char *) (pcCustomer -> _bName),
                                true) != errNoError)
                        {
                                Error err = CMSRepositoryErrors (__FILE__, __LINE__,
                                CMSREP_CMSREPOSITORY_MOVECONTENTINREPOSITORY_FAILED);
                                _ptSystemTracer -> trace (Tracer:: TRACER_LERRR,
                                        (const char *) err, __FILE__, __LINE__);
                        }

                        continue;
                }

                {
                        Message msg = CMSEngineMessages (__FILE__, __LINE__,
                                CMS_CMSENGINEPROCESSOR_ASSETTOINGEST,
                                2,
                                (const char *) (pcCustomer -> _bName),
                                (const char *) _bDirectoryEntry);
                        _ptSystemTracer -> trace (Tracer:: TRACER_LINFO,
                                (const char *) msg, __FILE__, __LINE__);
                }

                if (sendIngestAssetEvent (
                        pcCustomer,
                        (const char *) _bDestPathName,
                        llIngestionJobKey) != errNoError)
                {
                        Error err = CMSEngineErrors (__FILE__, __LINE__,
                                CMS_CMSENGINEPROCESSOR_SENDINGESTASSETEVENT_FAILED);
                        _ptSystemTracer -> trace (Tracer:: TRACER_LERRR,
                                (const char *) err, __FILE__, __LINE__);

                        if (IngestAssetThread:: updateIngestionJobStatusOnFileAndDB (
                                _pcrCMSRepository,
                                _pesEventsSet,
                                _pcCustomers,
                                "8",
                                false,
                                (const char *) err,
                                llIngestionJobKey,
                                -1,
                                _pcrCMSRepository -> getFTPRootRepository (),
                                pcCustomer -> _bName. str(),
                                pcCustomer -> _bDirectoryName. str(),
                                _plbWebServerLoadBalancer,
                                _pWebServerLocalIPAddress,
                                _ulWebServerTimeoutToWaitAnswerInSeconds,
                                _ptSystemTracer) != errNoError)
                        {
                                Error err = CMSEngineErrors (__FILE__, __LINE__,
                CMS_INGESTASSETTHREAD_UPDATEINGESTIONJOBSTATUSONFILEANDDB_FAILED);
                                _ptSystemTracer -> trace (Tracer:: TRACER_LERRR,
                                        (const char *) err, __FILE__, __LINE__);
                        }

                        if (_pcrCMSRepository -> moveContentInRepository (
                                (const char *) _bDestPathName,
                                CMSRepository:: CMSREP_REPOSITORYTYPE_ERRORS,
                                (const char *) (pcCustomer -> _bName),
                                true) != errNoError)
                        {
                                Error err = CMSRepositoryErrors (__FILE__, __LINE__,
                                        CMSREP_CMSREPOSITORY_MOVECONTENTINREPOSITORY_FAILED);
                                _ptSystemTracer -> trace (Tracer:: TRACER_LERRR,
                                        (const char *) err, __FILE__, __LINE__);
                        }

                        continue;
                }
                */

                ulCurrentCustomerIngestionsNumber++;

                if (ulCurrentCustomerIngestionsNumber >=
                    _ulMaxIngestionsNumberPerCustomerEachIngestionPeriod)
                    break;
            }

            FileIO:: closeDirectory (directory);
        }
        catch(...)
        {
            _logger->error(string("Error processing the Customer FTP")
                + ", customer->_name: " + customer->_name
                    );
        }
    }

    if (itCustomers == customers.end())
        _ulIngestionLastCustomerIndex	= 0;

}


/*
Error CheckIngestionThread:: sendIngestAssetEvent (
	Customer_p pcCustomer,
	const char *pMetaFilePathName,
	long long llIngestionJobKey)

{

	IngestAssetEvent_p			pevIngestAssetEvent;
	Event_p						pevEvent;


	if (_pesEventsSet -> getFreeEvent (
		CMSEngineEventsSet:: CMS_EVENTTYPE_INGESTASSETIDENTIFIER,
		&pevEvent) != errNoError)
	{
		Error err = EventsSetErrors (__FILE__, __LINE__,
			EVSET_EVENTSSET_GETFREEEVENT_FAILED);
		_ptSystemTracer -> trace (Tracer:: TRACER_LERRR,
			(const char *) err, __FILE__, __LINE__);

		return err;
	}

	pevIngestAssetEvent			= (IngestAssetEvent_p) pevEvent;

	if (pevIngestAssetEvent -> init (
		CMS_CHECKINGESTIONTHREAD_SOURCE,
		(const char *) _bMainProcessor,
		pcCustomer,
		pMetaFilePathName,
		llIngestionJobKey,
		_ptSystemTracer) != errNoError)
	{
		Error err = EventsSetErrors (__FILE__, __LINE__,
			EVSET_EVENT_INIT_FAILED);
		_ptSystemTracer -> trace (Tracer:: TRACER_LERRR,
			(const char *) err, __FILE__, __LINE__);

		if (_pesEventsSet -> releaseEvent (
			CMSEngineEventsSet:: CMS_EVENTTYPE_INGESTASSETIDENTIFIER,
			pevIngestAssetEvent) != errNoError)
		{
			Error err = EventsSetErrors (__FILE__, __LINE__,
				EVSET_EVENTSSET_RELEASEEVENT_FAILED);
			_ptSystemTracer -> trace (Tracer:: TRACER_LERRR,
				(const char *) err, __FILE__, __LINE__);
		}

		return err;
	}

	if (_pesEventsSet -> addEvent (pevIngestAssetEvent) != errNoError)
	{
		Error err = EventsSetErrors (__FILE__, __LINE__,
			EVSET_EVENTSSET_ADDEVENT_FAILED);
		_ptSystemTracer -> trace (Tracer:: TRACER_LERRR,
			(const char *) err, __FILE__, __LINE__);

		if (pevIngestAssetEvent -> finish () != errNoError)
		{
			Error err = EventsSetErrors (__FILE__, __LINE__,
				EVSET_EVENT_FINISH_FAILED);
			_ptSystemTracer -> trace (Tracer:: TRACER_LERRR,
				(const char *) err, __FILE__, __LINE__);
		}

		if (_pesEventsSet -> releaseEvent (
			CMSEngineEventsSet:: CMS_EVENTTYPE_INGESTASSETIDENTIFIER,
			pevIngestAssetEvent) != errNoError)
		{
			Error err = EventsSetErrors (__FILE__, __LINE__,
				EVSET_EVENTSSET_RELEASEEVENT_FAILED);
			_ptSystemTracer -> trace (Tracer:: TRACER_LERRR,
				(const char *) err, __FILE__, __LINE__);
		}

		return err;
	}


	return errNoError;
}
*/
