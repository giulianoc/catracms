/*
 * To change this license header, choose License Headers in Project Properties.
 * To change this template file, choose Tools | Templates
 * and open the template in the editor.
 */

/* 
 * File:   API.cpp
 * Author: giuliano
 * 
 * Created on February 18, 2018, 1:27 AM
 */

#include "JSONUtils.h"
#include <fstream>
#include <sstream>
#include <regex>
#include "spdlog/sinks/stdout_color_sinks.h"
#include "spdlog/sinks/daily_file_sink.h"
#include "spdlog/sinks/rotating_file_sink.h"
#include <curlpp/cURLpp.hpp>
#include <curlpp/Easy.hpp>
#include <curlpp/Options.hpp>
#include <curlpp/Exception.hpp>
#include <curlpp/Infos.hpp>
#include "catralibraries/Convert.h"
#include "catralibraries/LdapWrapper.h"
#include "Validator.h"
#include "EMailSender.h"
#include "catralibraries/Encrypt.h"
#include <openssl/md5.h>

#include <libxml/tree.h>
#include <libxml/parser.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>

#include "API.h"

int main(int argc, char** argv) 
{

	try
	{
		// Init libxml
		{
			xmlInitParser();
			LIBXML_TEST_VERSION
		}

		const char* configurationPathName = getenv("MMS_CONFIGPATHNAME");
		if (configurationPathName == nullptr)
		{
			cerr << "MMS API: the MMS_CONFIGPATHNAME environment variable is not defined" << endl;
        
			return 1;
		}
    
		Json::Value configuration = APICommon::loadConfigurationFile(configurationPathName);
    
		string logPathName =  configuration["log"]["api"].get("pathName", "XXX").asString();
		string logType =  configuration["log"]["api"].get("type", "").asString();
		bool stdout =  JSONUtils::asBool(configuration["log"]["api"], "stdout", false);
    
		std::vector<spdlog::sink_ptr> sinks;
		{
			if(logType == "daily")
			{
				int logRotationHour = JSONUtils::asInt(configuration["log"]["api"]["daily"],
					"rotationHour", 1);
				int logRotationMinute = JSONUtils::asInt(configuration["log"]["api"]["daily"],
					"rotationMinute", 1);

				auto dailySink = make_shared<spdlog::sinks::daily_file_sink_mt> (logPathName.c_str(),
					logRotationHour, logRotationMinute);
				sinks.push_back(dailySink);
			}
			else if(logType == "rotating")
			{
				int64_t maxSizeInKBytes = JSONUtils::asInt64(configuration["log"]["encoder"]["rotating"],
					"maxSizeInKBytes", 1000);
				int maxFiles = JSONUtils::asInt(configuration["log"]["api"]["rotating"],
					"maxFiles", 10);

				auto rotatingSink = make_shared<spdlog::sinks::rotating_file_sink_mt> (logPathName.c_str(),
					maxSizeInKBytes * 1000, maxFiles);
				sinks.push_back(rotatingSink);
			}

			if (stdout)
			{
				auto stdoutSink = make_shared<spdlog::sinks::stdout_color_sink_mt>();
				sinks.push_back(stdoutSink);
			}
		}

		auto logger = std::make_shared<spdlog::logger>("API", begin(sinks), end(sinks));
		// shared_ptr<spdlog::logger> logger = spdlog::stdout_logger_mt("API");
		// shared_ptr<spdlog::logger> logger = spdlog::daily_logger_mt("API", logPathName.c_str(), 11, 20);
    
		// trigger flush if the log severity is error or higher
		logger->flush_on(spdlog::level::trace);
    
		string logLevel =  configuration["log"]["api"].get("level", "XXX").asString();
		if (logLevel == "debug")
			spdlog::set_level(spdlog::level::debug); // trace, debug, info, warn, err, critical, off
		else if (logLevel == "info")
			spdlog::set_level(spdlog::level::info); // trace, debug, info, warn, err, critical, off
		else if (logLevel == "err")
			spdlog::set_level(spdlog::level::err); // trace, debug, info, warn, err, critical, off
		string pattern =  configuration["log"]["api"].get("pattern", "XXX").asString();
		spdlog::set_pattern(pattern);

		// globally register the loggers so so the can be accessed using spdlog::get(logger_name)
		// spdlog::register_logger(logger);

		/*
		// the log is written in the apache error log (stderr)
		_logger = spdlog::stderr_logger_mt("API");

		// make sure only responses are written to the standard output
		spdlog::set_level(spdlog::level::trace);
    
		spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%l] [tid %t] %v");
    
		// globally register the loggers so so the can be accessed using spdlog::get(logger_name)
		// spdlog::register_logger(logger);
		*/

		size_t dbPoolSize = JSONUtils::asInt(configuration["database"], "apiPoolSize", 5);
		logger->info(__FILEREF__ + "Configuration item"
			+ ", database->poolSize: " + to_string(dbPoolSize)
		);
		logger->info(__FILEREF__ + "Creating MMSEngineDBFacade"
            );
		shared_ptr<MMSEngineDBFacade> mmsEngineDBFacade = make_shared<MMSEngineDBFacade>(
            configuration, dbPoolSize, logger);

		logger->info(__FILEREF__ + "Creating MMSStorage"
			);
		shared_ptr<MMSStorage> mmsStorage = make_shared<MMSStorage>(
			configuration, logger);

		FCGX_Init();

		int threadsNumber = JSONUtils::asInt(configuration["api"], "threadsNumber", 1);
		logger->info(__FILEREF__ + "Configuration item"
			+ ", api->threadsNumber: " + to_string(threadsNumber)
		);

		mutex fcgiAcceptMutex;
		API::FileUploadProgressData fileUploadProgressData;

		vector<shared_ptr<API>> apis;
		vector<thread> apiThreads;

		for (int threadIndex = 0; threadIndex < threadsNumber; threadIndex++)
		{
			shared_ptr<API> api = make_shared<API>(configuration, 
                mmsEngineDBFacade,
				mmsStorage,
                &fcgiAcceptMutex,
                &fileUploadProgressData,
                logger
				);

			apis.push_back(api);
			apiThreads.push_back(thread(&API::operator(), api));
		}

		// shutdown should be managed in some way:
		// - mod_fcgid send just one shutdown, so only one thread will go down
		// - mod_fastcgi ???
		if (threadsNumber > 0)
		{
			thread fileUploadProgressThread(&API::fileUploadProgressCheck, apis[0]);
        
			apiThreads[0].join();
        
			apis[0]->stopUploadFileProgressThread();
		}

		logger->info(__FILEREF__ + "API shutdown");

		// libxml
		{
			// Shutdown libxml
			xmlCleanupParser();

			// this is to debug memory for regression tests
			xmlMemoryDump();
		}
	}
    catch(sql::SQLException se)
    {
        cerr << __FILEREF__ + "main failed. SQL exception"
            + ", se.what(): " + se.what()
        ;

        // throw se;
		return 1;
    }
    catch(runtime_error e)
    {
        cerr << __FILEREF__ + "main failed"
            + ", e.what(): " + e.what()
        ;

        // throw e;
		return 1;
    }
    catch(exception e)
    {
        cerr << __FILEREF__ + "main failed"
            + ", e.what(): " + e.what()
        ;

        // throw runtime_error(errorMessage);
		return 1;
    }

    return 0;
}

API::API(Json::Value configuration, 
            shared_ptr<MMSEngineDBFacade> mmsEngineDBFacade,
			shared_ptr<MMSStorage> mmsStorage,
            mutex* fcgiAcceptMutex,
            FileUploadProgressData* fileUploadProgressData,
            shared_ptr<spdlog::logger> logger)
    :APICommon(configuration, 
            mmsEngineDBFacade,
            fcgiAcceptMutex,
            logger) 
{
	_mmsStorage = mmsStorage;

    string encodingPriority =  _configuration["api"].get("encodingPriorityWorkspaceDefaultValue", "XXX").asString();
    _logger->info(__FILEREF__ + "Configuration item"
        + ", api->encodingPriorityWorkspaceDefaultValue: " + encodingPriority
    );
    try
    {
        _encodingPriorityWorkspaceDefaultValue = MMSEngineDBFacade::toEncodingPriority(encodingPriority);    // it generate an exception in case of wrong string
    }
    catch(exception e)
    {
        _logger->error(__FILEREF__ + "Configuration item is wrong. 'low' encoding priority is set"
            + ", api->encodingPriorityWorkspaceDefaultValue: " + encodingPriority
        );

        _encodingPriorityWorkspaceDefaultValue = MMSEngineDBFacade::EncodingPriority::Low;
    }

	_maxPageSize = JSONUtils::asInt(configuration["database"], "maxPageSize", 5);
	logger->info(__FILEREF__ + "Configuration item"
		+ ", database->maxPageSize: " + to_string(_maxPageSize)
	);

    string encodingPeriod =  _configuration["api"].get("encodingPeriodWorkspaceDefaultValue", "XXX").asString();
    _logger->info(__FILEREF__ + "Configuration item"
        + ", api->encodingPeriodWorkspaceDefaultValue: " + encodingPeriod
    );
    if (encodingPeriod == "daily")
        _encodingPeriodWorkspaceDefaultValue = MMSEngineDBFacade::EncodingPeriod::Daily;
    else
        _encodingPeriodWorkspaceDefaultValue = MMSEngineDBFacade::EncodingPeriod::Daily;

    _maxIngestionsNumberWorkspaceDefaultValue = JSONUtils::asInt(_configuration["api"], "maxIngestionsNumberWorkspaceDefaultValue", 0);
    _logger->info(__FILEREF__ + "Configuration item"
        + ", api->maxIngestionsNumberWorkspaceDefaultValue: " + to_string(_maxIngestionsNumberWorkspaceDefaultValue)
    );
    _maxStorageInMBWorkspaceDefaultValue = JSONUtils::asInt(_configuration["api"], "maxStorageInMBWorkspaceDefaultValue", 0);
    _logger->info(__FILEREF__ + "Configuration item"
        + ", api->maxStorageInMBWorkspaceDefaultValue: " + to_string(_maxStorageInMBWorkspaceDefaultValue)
    );

    _apiProtocol =  _configuration["api"].get("protocol", "XXX").asString();
    _logger->info(__FILEREF__ + "Configuration item"
        + ", api->protocol: " + _apiProtocol
    );
    _apiHostname =  _configuration["api"].get("hostname", "XXX").asString();
    _logger->info(__FILEREF__ + "Configuration item"
        + ", api->hostname: " + _apiHostname
    );
    _apiPort = JSONUtils::asInt(_configuration["api"], "port", 0);
    _logger->info(__FILEREF__ + "Configuration item"
        + ", api->port: " + to_string(_apiPort)
    );

    Json::Value api = _configuration["api"];
    // _binaryBufferLength             = api["binary"].get("binaryBufferLength", "XXX").asInt();
    // _logger->info(__FILEREF__ + "Configuration item"
    //    + ", api->binary->binaryBufferLength: " + to_string(_binaryBufferLength)
    // );
    _progressUpdatePeriodInSeconds  = JSONUtils::asInt(api["binary"], "progressUpdatePeriodInSeconds", 0);
    _logger->info(__FILEREF__ + "Configuration item"
        + ", api->binary->progressUpdatePeriodInSeconds: " + to_string(_progressUpdatePeriodInSeconds)
    );
    _webServerPort  = JSONUtils::asInt(api["binary"], "webServerPort", 0);
    _logger->info(__FILEREF__ + "Configuration item"
        + ", api->binary->webServerPort: " + to_string(_webServerPort)
    );
    _maxProgressCallFailures  = JSONUtils::asInt(api["binary"], "maxProgressCallFailures", 0);
    _logger->info(__FILEREF__ + "Configuration item"
        + ", api->binary->maxProgressCallFailures: " + to_string(_maxProgressCallFailures)
    );
    _progressURI  = api["binary"].get("progressURI", "XXX").asString();
    _logger->info(__FILEREF__ + "Configuration item"
        + ", api->binary->progressURI: " + _progressURI
    );
    
    _defaultTTLInSeconds  = JSONUtils::asInt(api["delivery"], "defaultTTLInSeconds", 60);
    _logger->info(__FILEREF__ + "Configuration item"
        + ", api->delivery->defaultTTLInSeconds: " + to_string(_defaultTTLInSeconds)
    );

    _defaultMaxRetries  = JSONUtils::asInt(api["delivery"], "defaultMaxRetries", 60);
    _logger->info(__FILEREF__ + "Configuration item"
        + ", api->delivery->defaultMaxRetries: " + to_string(_defaultMaxRetries)
    );

    _defaultRedirect  = JSONUtils::asBool(api["delivery"], "defaultRedirect", true);
    _logger->info(__FILEREF__ + "Configuration item"
        + ", api->delivery->defaultRedirect: " + to_string(_defaultRedirect)
    );
    
    _deliveryProtocol  = api["delivery"].get("deliveryProtocol", "XXX").asString();
    _logger->info(__FILEREF__ + "Configuration item"
        + ", api->delivery->deliveryProtocol: " + _deliveryProtocol
    );
    _deliveryHost_authorizationThroughParameter  = api["delivery"].get("deliveryHost_authorizationThroughParameter", "").asString();
    _logger->info(__FILEREF__ + "Configuration item"
        + ", api->delivery->deliveryHost_authorizationThroughParameter: " + _deliveryHost_authorizationThroughParameter
    );
    _deliveryHost_authorizationThroughPath  = api["delivery"].get("deliveryHost_authorizationThroughPath", "").asString();
    _logger->info(__FILEREF__ + "Configuration item"
		+ ", api->delivery->deliveryHost_authorizationThroughPath: " + _deliveryHost_authorizationThroughPath
	);

    _ldapEnabled  = JSONUtils::asBool(api["activeDirectory"], "enabled", false);
    _logger->info(__FILEREF__ + "Configuration item"
        + ", api->activeDirectory->enabled: " + to_string(_ldapEnabled)
    );
    _ldapURL  = api["activeDirectory"].get("ldapURL", "XXX").asString();
    _logger->info(__FILEREF__ + "Configuration item"
        + ", api->activeDirectory->ldapURL: " + _ldapURL
    );
    _ldapCertificatePathName  = api["activeDirectory"].get("certificatePathName", "XXX").asString();
    _logger->info(__FILEREF__ + "Configuration item"
        + ", api->activeDirectory->certificatePathName: " + _ldapCertificatePathName
    );
    _ldapManagerUserName  = api["activeDirectory"].get("managerUserName", "XXX").asString();
    _logger->info(__FILEREF__ + "Configuration item"
        + ", api->activeDirectory->managerUserName: " + _ldapManagerUserName
    );
    _ldapManagerPassword  = api["activeDirectory"].get("managerPassword", "XXX").asString();
    _logger->info(__FILEREF__ + "Configuration item"
        + ", api->activeDirectory->managerPassword: " + _ldapManagerPassword
    );
    _ldapBaseDn  = api["activeDirectory"].get("baseDn", "XXX").asString();
    _logger->info(__FILEREF__ + "Configuration item"
        + ", api->activeDirectory->baseDn: " + _ldapBaseDn
    );
	_ldapDefaultWorkspaceKeys	= api["activeDirectory"].get("defaultWorkspaceKeys", 0).asString();
	_logger->info(__FILEREF__ + "Configuration item"
		+ ", api->activeDirectory->defaultWorkspaceKeys: " + _ldapDefaultWorkspaceKeys
	);

	_savingGEOUserInfo = JSONUtils::asBool(_configuration["mms"]["geoService"], "savingGEOUserInfo", false);
	if (_savingGEOUserInfo)
	{
		_geoServiceURL = _configuration["mms"]["geoService"].get("geoServiceURL", "").asString();
		_geoServiceTimeoutInSeconds = JSONUtils::asInt(_configuration["mms"]["geoService"], "geoServiceTimeoutInSeconds", 10);
	}

	/*
    _ffmpegEncoderProtocol = _configuration["ffmpeg"].get("encoderProtocol", "").asString();
    _logger->info(__FILEREF__ + "Configuration item"
        + ", ffmpeg->encoderProtocol: " + _ffmpegEncoderProtocol
    );
    _ffmpegEncoderPort = JSONUtils::asInt(_configuration["ffmpeg"], "encoderPort", 0);
    _logger->info(__FILEREF__ + "Configuration item"
        + ", ffmpeg->encoderPort: " + to_string(_ffmpegEncoderPort)
    );
	*/
    _ffmpegEncoderUser = _configuration["ffmpeg"].get("encoderUser", "").asString();
    _logger->info(__FILEREF__ + "Configuration item"
        + ", ffmpeg->encoderUser: " + _ffmpegEncoderUser
    );
    _ffmpegEncoderPassword = _configuration["ffmpeg"].get("encoderPassword", "").asString();
    _logger->info(__FILEREF__ + "Configuration item"
        + ", ffmpeg->encoderPassword: " + "..."
    );
    _ffmpegEncoderTimeoutInSeconds = JSONUtils::asInt(_configuration["ffmpeg"], "encoderTimeoutInSeconds", 120);
    _logger->info(__FILEREF__ + "Configuration item"
        + ", ffmpeg->encoderTimeoutInSeconds: " + to_string(_ffmpegEncoderTimeoutInSeconds)
    );
    _ffmpegEncoderKillEncodingURI = _configuration["ffmpeg"].get("encoderKillEncodingURI", "").asString();
    _logger->info(__FILEREF__ + "Configuration item"
        + ", ffmpeg->encoderKillEncodingURI: " + _ffmpegEncoderKillEncodingURI
    );

	_maxSecondsToWaitAPIIngestionLock  = JSONUtils::asInt(_configuration["mms"]["locks"], "maxSecondsToWaitAPIIngestionLock", 0);
	_logger->info(__FILEREF__ + "Configuration item"
		+ ", mms->locks->maxSecondsToWaitAPIIngestionLock: " + to_string(_maxSecondsToWaitAPIIngestionLock)
	);

    _fileUploadProgressData     = fileUploadProgressData;
    _fileUploadProgressThreadShutdown       = false;
}

API::~API() {
}

/*
void API::getBinaryAndResponse(
        string requestURI,
        string requestMethod,
        string xCatraMMSResumeHeader,
        unordered_map<string, string> queryParameters,
        tuple<int64_t,shared_ptr<Workspace>,bool,bool,bool>& userKeyWorkspaceAndFlags,
        unsigned long contentLength
)
{
    _logger->error(__FILEREF__ + "API application is able to manage ONLY NON-Binary requests");
    
    string errorMessage = string("Internal server error");
    _logger->error(__FILEREF__ + errorMessage);

    sendError(500, errorMessage);

    throw runtime_error(errorMessage);
}
*/

void API::manageRequestAndResponse(
        FCGX_Request& request,
        string requestURI,
        string requestMethod,
        unordered_map<string, string> queryParameters,
        bool basicAuthenticationPresent,
        tuple<int64_t,shared_ptr<Workspace>, bool, bool, bool, bool, bool, bool, bool, bool, bool, bool, bool, bool>&
			userKeyWorkspaceAndFlags,
		string apiKey,
        unsigned long contentLength,
        string requestBody,
        unordered_map<string, string>& requestDetails
)
{

    int64_t userKey;
    shared_ptr<Workspace> workspace;
    bool admin;
    bool createRemoveWorkspace;
    bool ingestWorkflow;
    bool createProfiles;
    bool deliveryAuthorization;
    bool shareWorkspace;
    bool editMedia;
    bool editConfiguration;
    bool killEncoding;
    bool cancelIngestionJob_;
    bool editEncodersPool;
    bool applicationRecorder;

    if (basicAuthenticationPresent)
    {
        tie(userKey, workspace, admin, createRemoveWorkspace, ingestWorkflow, createProfiles,
				deliveryAuthorization, shareWorkspace, editMedia, editConfiguration, killEncoding,
				cancelIngestionJob_, editEncodersPool, applicationRecorder) 
                = userKeyWorkspaceAndFlags;

        _logger->info(__FILEREF__ + "Received manageRequestAndResponse"
            + ", requestURI: " + requestURI
            + ", requestMethod: " + requestMethod
            + ", contentLength: " + to_string(contentLength)
            + ", userKey: " + to_string(userKey)
            + ", workspace->_name: " + workspace->_name
            + ", requestBody: " + requestBody
            + ", admin: " + to_string(admin)
            + ", createRemoveWorkspace: " + to_string(createRemoveWorkspace)
            + ", ingestWorkflow: " + to_string(ingestWorkflow)
            + ", createProfiles: " + to_string(createProfiles)
            + ", deliveryAuthorization: " + to_string(deliveryAuthorization)
            + ", shareWorkspace: " + to_string(shareWorkspace)
            + ", editMedia: " + to_string(editMedia)
            + ", editConfiguration: " + to_string(editConfiguration)
            + ", killEncoding: " + to_string(killEncoding)
            + ", cancelIngestionJob: " + to_string(cancelIngestionJob_)
            + ", editEncodersPool: " + to_string(editEncodersPool)
            + ", applicationRecorder: " + to_string(applicationRecorder)
        );        
    }

    auto methodIt = queryParameters.find("method");
    if (methodIt == queryParameters.end())
    {
        string errorMessage = string("The 'method' parameter is not found");
        _logger->error(__FILEREF__ + errorMessage);

        sendError(request, 400, errorMessage);

        throw runtime_error(errorMessage);
    }
    string method = methodIt->second;

    if (!basicAuthenticationPresent)
    {
        _logger->info(__FILEREF__ + "Received manageRequestAndResponse"
            + ", requestURI: " + requestURI
            + ", requestMethod: " + requestMethod
            + ", contentLength: " + to_string(contentLength)
            + ", method: " + method
			// next is to avoid to log the password
            + (method == "login" ? ", requestBody: ..." : (", requestBody: " + requestBody))
        );
    }

    if (method == "status")
    {
        try
        {            
            string responseBody = string("{ ")
                    + "\"status\": \"API server up and running\" "
                    + "}";

            sendSuccess(request, 200, responseBody);
        }
        catch(exception e)
        {
            _logger->error(__FILEREF__ + "status failed"
                + ", requestBody: " + requestBody
                + ", e.what(): " + e.what()
            );

            string errorMessage = string("Internal server error");
            _logger->error(__FILEREF__ + errorMessage);

            sendError(request, 500, errorMessage);

            throw runtime_error(errorMessage);
        }
    }
    else if (method == "binaryAuthorization")
    {
        // since we are here, for sure user is authorized
        
		auto binaryVirtualHostNameIt = queryParameters.find("binaryVirtualHostName");
		auto binaryListenHostIt = queryParameters.find("binaryListenHost");

        // retrieve the HTTP_X_ORIGINAL_METHOD to retrieve the progress id (set in the nginx server configuration)
        auto progressIdIt = requestDetails.find("HTTP_X_ORIGINAL_METHOD");
        auto originalURIIt = requestDetails.find("HTTP_X_ORIGINAL_URI");
        if (binaryVirtualHostNameIt != queryParameters.end()
				&& binaryListenHostIt != queryParameters.end()
				&& progressIdIt != requestDetails.end()
				&& originalURIIt != requestDetails.end())
        {
            int ingestionJobKeyIndex = originalURIIt->second.find_last_of("/");
            if (ingestionJobKeyIndex != string::npos)
            {
                try
                {
                    struct FileUploadProgressData::RequestData requestData;

                    requestData._progressId = progressIdIt->second;
                    requestData._binaryListenHost = binaryListenHostIt->second;
                    requestData._binaryVirtualHostName = binaryVirtualHostNameIt->second;
					// requestData._binaryListenIp = binaryVirtualHostNameIt->second;
                    requestData._ingestionJobKey = stoll(originalURIIt->second.substr(ingestionJobKeyIndex + 1));
                    requestData._lastPercentageUpdated = 0;
                    requestData._callFailures = 0;

                    // Content-Range: bytes 0-99999/100000
                    requestData._contentRangePresent = false;
                    requestData._contentRangeStart  = -1;
                    requestData._contentRangeEnd  = -1;
                    requestData._contentRangeSize  = -1;
                    auto contentRangeIt = requestDetails.find("HTTP_CONTENT_RANGE");
                    if (contentRangeIt != requestDetails.end())
                    {
                        string contentRange = contentRangeIt->second;
                        try
                        {
                            parseContentRange(contentRange,
                                requestData._contentRangeStart,
                                requestData._contentRangeEnd,
                                requestData._contentRangeSize);

                            requestData._contentRangePresent = true;                
                        }
                        catch(exception e)
                        {
                            string errorMessage = string("Content-Range is not well done. Expected format: 'Content-Range: bytes <start>-<end>/<size>'")
                                + ", contentRange: " + contentRange
                            ;
                            _logger->error(__FILEREF__ + errorMessage);

                            sendError(request, 500, errorMessage);

                            throw runtime_error(errorMessage);            
                        }
                    }

                    _logger->info(__FILEREF__ + "Content-Range details"
                        + ", contentRangePresent: " + to_string(requestData._contentRangePresent)
                        + ", contentRangeStart: " + to_string(requestData._contentRangeStart)
                        + ", contentRangeEnd: " + to_string(requestData._contentRangeEnd)
                        + ", contentRangeSize: " + to_string(requestData._contentRangeSize)
                    );

                    lock_guard<mutex> locker(_fileUploadProgressData->_mutex);                    

                    _fileUploadProgressData->_filesUploadProgressToBeMonitored.push_back(requestData);
                    _logger->info(__FILEREF__ + "Added upload file progress to be monitored"
                        + ", _progressId: " + requestData._progressId
                        + ", _binaryVirtualHostName: " + requestData._binaryVirtualHostName
                        + ", _binaryListenHost: " + requestData._binaryListenHost
                    );
                }
                catch (exception e)
                {
                    _logger->error(__FILEREF__ + "ProgressId not found"
                        + ", progressIdIt->second: " + progressIdIt->second
                    );
                }
            }
        }        
        
        string responseBody;
        sendSuccess(request, 200, responseBody);
    }
    else if (method == "deliveryAuthorizationThroughParameter")
    {
        // retrieve the HTTP_X_ORIGINAL_METHOD to retrieve the token to be checked (set in the nginx server configuration)
        try
        {
			auto tokenIt = requestDetails.find("HTTP_X_ORIGINAL_METHOD");
			auto originalURIIt = requestDetails.find("HTTP_X_ORIGINAL_URI");
			if (tokenIt == requestDetails.end()
				|| originalURIIt == requestDetails.end())
			{
				string errorMessage = string("deliveryAuthorization, not authorized")
					+ ", token: " + (tokenIt != requestDetails.end() ? tokenIt->second : "null")
					+ ", URI: " + (originalURIIt != requestDetails.end() ? originalURIIt->second : "null")
                          ;
				_logger->warn(__FILEREF__ + errorMessage);

				throw runtime_error(errorMessage);
			}

			string contentURI = originalURIIt->second;
			size_t endOfURIIndex = contentURI.find_last_of("?");
			if (endOfURIIndex == string::npos)
			{
				string errorMessage = string("Wrong URI format")
					+ ", contentURI: " + contentURI
				;
				_logger->warn(__FILEREF__ + errorMessage);

				throw runtime_error(errorMessage);
			}
			contentURI = contentURI.substr(0, endOfURIIndex);

			string tokenParameter = tokenIt->second;

			_logger->info(__FILEREF__ + "Calling checkDeliveryAuthorizationThroughParameter"
				+ ", contentURI: " + contentURI
				+ ", tokenParameter: " + tokenParameter
			);

			checkDeliveryAuthorizationThroughParameter(contentURI, tokenParameter);

			string responseBody;
			sendSuccess(request, 200, responseBody);
        }
        catch(runtime_error e)
        {
            string errorMessage = string("Not authorized");
            _logger->warn(__FILEREF__ + errorMessage);

			string responseBody;
			sendError(request, 403, errorMessage);
        }
        catch(exception e)
        {
            string errorMessage = string("Not authorized: exception managing token");
            _logger->warn(__FILEREF__ + errorMessage);

            sendError(request, 500, errorMessage);
        }
    }
    else if (method == "deliveryAuthorizationThroughPath")
    {
        // retrieve the HTTP_X_ORIGINAL_METHOD to retrieve the token to be checked (set in the nginx server configuration)
        try
        {
			auto originalURIIt = requestDetails.find("HTTP_X_ORIGINAL_URI");
			if (originalURIIt == requestDetails.end())
			{
				string errorMessage = string("deliveryAuthorization, not authorized")
					+ ", URI: " + (originalURIIt != requestDetails.end() ? originalURIIt->second : "null")
                          ;
				_logger->warn(__FILEREF__ + errorMessage);

				throw runtime_error(errorMessage);
			}
			string contentURI = originalURIIt->second;

			_logger->info(__FILEREF__ + "deliveryAuthorizationThroughPath. Calling checkAuthorizationThroughPath"
				+ ", contentURI: " + contentURI
			);

			checkDeliveryAuthorizationThroughPath(contentURI);

			string responseBody;
			sendSuccess(request, 200, responseBody);
        }
        catch(runtime_error e)
        {
            string errorMessage = string("Not authorized");
            _logger->warn(__FILEREF__ + errorMessage);

			string responseBody;
			sendError(request, 403, errorMessage);
        }
        catch(exception e)
        {
            string errorMessage = string("Not authorized: exception managing token");
            _logger->warn(__FILEREF__ + errorMessage);

            sendError(request, 500, errorMessage);
        }
    }
    else if (method == "manageHTTPStreamingManifest_authorizationThroughParameter")
    {
        try
        {
            auto tokenIt = queryParameters.find("token");
            if (tokenIt == queryParameters.end())
			{
				string errorMessage = string("Not authorized: token parameter not present")
					;
				_logger->warn(__FILEREF__ + errorMessage);

				throw runtime_error(errorMessage);
			}

			// we could have:
			//		- master manifest, token parameter: <token>---
			//		- secondary manifest (that has to be treated as a .ts delivery), token parameter:
			//			<encryption of 'manifestLine+++token'>---<cookie: encription of 'token'>
			bool secondaryManifest;
			int64_t tokenComingFromURL;

			bool isNumber = !(tokenIt->second).empty()
				&& find_if(
					(tokenIt->second).begin(),
					(tokenIt->second).end(),
					[](unsigned char c) {
						return !isdigit(c);
					})
					== (tokenIt->second).end();
			if (isNumber)
			{
				secondaryManifest = false;
				tokenComingFromURL = stoll(tokenIt->second);
			}
			else
			{
				secondaryManifest = true;
				// tokenComingFromURL will be initialized in the next statement
			}
			_logger->info(__FILEREF__ + "manageHTTPStreamingManifest"
				+ ", analizing the token " + tokenIt->second
				+ ", isNumber: " + to_string(isNumber)
				+ ", secondaryManifest: " + to_string(secondaryManifest)
			);

			string contentURI;
			{
				size_t endOfURIIndex = requestURI.find_last_of("?");
				if (endOfURIIndex == string::npos)
				{
					string errorMessage = string("Wrong URI format")
						+ ", requestURI: " + requestURI
					;
					_logger->info(__FILEREF__ + errorMessage);

					throw runtime_error(errorMessage);
				}
				contentURI = requestURI.substr(0, endOfURIIndex);
			}

			if (secondaryManifest)
			{
				auto cookieIt = queryParameters.find("cookie");
				if (cookieIt == queryParameters.end())
				{
					string errorMessage = string("The 'cookie' parameter is not found");
					_logger->error(__FILEREF__ + errorMessage);

					throw runtime_error(errorMessage);
				}
				string cookie = cookieIt->second;

				string tokenParameter = tokenIt->second + "---" + cookie;
				_logger->info(__FILEREF__ + "Calling checkDeliveryAuthorizationThroughParameter"
					+ ", contentURI: " + contentURI
					+ ", tokenParameter: " + tokenParameter
				);
				tokenComingFromURL = checkDeliveryAuthorizationThroughParameter(contentURI, tokenParameter);
			}
			else
			{
				// cookie parameter is added inside catramms.nginx
				string mmsInfoCookie;
				auto cookieIt = queryParameters.find("cookie");
				if (cookieIt != queryParameters.end())
					mmsInfoCookie = cookieIt->second;

				_logger->info(__FILEREF__ + "manageHTTPStreamingManifest"
					+ ", tokenComingFromURL: " + to_string(tokenComingFromURL)
					+ ", mmsInfoCookie: " + mmsInfoCookie
				);

				if (mmsInfoCookie == "")
				{
					if (!_mmsEngineDBFacade->checkDeliveryAuthorization(tokenComingFromURL, contentURI))
					{
						string errorMessage = string("Not authorized: token invalid")
							+ ", contentURI: " + contentURI
							+ ", tokenComingFromURL: " + to_string(tokenComingFromURL)
						;
						_logger->info(__FILEREF__ + errorMessage);

						throw runtime_error(errorMessage);
					}

					_logger->info(__FILEREF__ + "token authorized"
						+ ", tokenComingFromURL: " + to_string(tokenComingFromURL)
					);
				}
				else
				{
					string sTokenComingFromCookie = Encrypt::decrypt(mmsInfoCookie);
					int64_t tokenComingFromCookie = stoll(sTokenComingFromCookie);

					if (tokenComingFromCookie != tokenComingFromURL)
					{
						string errorMessage = string("cookie invalid, let's check the token")
							+ ", tokenComingFromCookie: " + to_string(tokenComingFromCookie)
							+ ", tokenComingFromURL: " + to_string(tokenComingFromURL)
						;
						_logger->info(__FILEREF__ + errorMessage);

						if (!_mmsEngineDBFacade->checkDeliveryAuthorization(tokenComingFromURL, contentURI))
						{
							string errorMessage = string("Not authorized: token invalid")
								+ ", contentURI: " + contentURI
								+ ", tokenComingFromURL: " + to_string(tokenComingFromURL)
							;
							_logger->info(__FILEREF__ + errorMessage);

							throw runtime_error(errorMessage);
						}

						_logger->info(__FILEREF__ + "token authorized"
							+ ", tokenComingFromURL: " + to_string(tokenComingFromURL)
						);
					}
					else
					{
						_logger->info(__FILEREF__ + "cookie authorized"
							+ ", mmsInfoCookie: " + mmsInfoCookie
						);
					}
				}
			}

			// manifest authorized

			{
				string contentType;

				string m3u8Extension(".m3u8");
				if (contentURI.size() >= m3u8Extension.size() && 0 == contentURI.compare(
					contentURI.size()-m3u8Extension.size(), m3u8Extension.size(), m3u8Extension))
					contentType = "Content-type: application/x-mpegURL";
				else	// dash
					contentType = "Content-type: application/dash+xml";
				string cookieName = "mmsInfo";

				string responseBody;
				{
					string manifestPathFileName = _mmsStorage->getMMSRootRepository()
						+ contentURI.substr(1);

					_logger->info(__FILEREF__ + "Reading manifest file"
						+ ", manifestPathFileName: " + manifestPathFileName
					);

					if (!FileIO::isFileExisting (manifestPathFileName.c_str()))
					{
						string errorMessage = string("manifest file not existing")
							+ ", manifestPathFileName: " + manifestPathFileName
							;
						_logger->error(__FILEREF__ + errorMessage);

						throw runtime_error(errorMessage);
					}

					if (contentURI.size() >= m3u8Extension.size() && 0 == contentURI.compare(
						contentURI.size()-m3u8Extension.size(), m3u8Extension.size(), m3u8Extension))
					{
						std::ifstream manifestFile;

						manifestFile.open(manifestPathFileName, ios::in);
						if (!manifestFile.is_open())
						{
							string errorMessage = string("Not authorized: manifest file not opened")
								+ ", manifestPathFileName: " + manifestPathFileName
								;
							_logger->info(__FILEREF__ + errorMessage);

							throw runtime_error(errorMessage);
						}

						string manifestLine;
						string tsExtension = ".ts";
						string m3u8Extension = ".m3u8";
						string m3u8ExtXMedia = "#EXT-X-MEDIA";
						string endLine = "\n";
						while(getline(manifestFile, manifestLine))
						{
							if (manifestLine[0] != '#' &&

								// end with
								manifestLine.size() >= tsExtension.size()
								&& 0 == manifestLine.compare(manifestLine.size()-tsExtension.size(),
									tsExtension.size(), tsExtension)
							)
							{
								/*
								_logger->info(__FILEREF__ + "Creation token parameter for ts"
									+ ", manifestLine: " + manifestLine
									+ ", tokenComingFromURL: " + to_string(tokenComingFromURL)
								);
								*/
								string auth = Encrypt::encrypt(manifestLine + "+++" + to_string(tokenComingFromURL));
								responseBody += (manifestLine + "?token=" + auth + endLine);
							}
							else if (manifestLine[0] != '#' &&

								// end with
								manifestLine.size() >= m3u8Extension.size()
								&& 0 == manifestLine.compare(manifestLine.size()-m3u8Extension.size(),
									m3u8Extension.size(), m3u8Extension)
							)
							{
								// scenario where we have several .m3u8 manifest files
								/*
								_logger->info(__FILEREF__ + "Creation token parameter for m3u8"
									+ ", manifestLine: " + manifestLine
									+ ", tokenComingFromURL: " + to_string(tokenComingFromURL)
								);
								*/
								string auth = Encrypt::encrypt(manifestLine + "+++" + to_string(tokenComingFromURL));
								responseBody += (manifestLine + "?token=" + auth + endLine);
							}
							// start with
							else if (manifestLine.size() >= m3u8ExtXMedia.size() &&
								0 == manifestLine.compare(0, m3u8ExtXMedia.size(), m3u8ExtXMedia))
							{
								// #EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID="audio",LANGUAGE="eng",NAME="eng",AUTOSELECT=YES, DEFAULT=YES,URI="eng/1247999_384641.m3u8"
								string temp = "URI=\"";
								size_t uriStartIndex = manifestLine.find(temp);
								if (uriStartIndex != string::npos)
								{
									uriStartIndex += temp.size();
									size_t uriEndIndex = uriStartIndex;
									while(manifestLine[uriEndIndex] != '\"'
											&& uriEndIndex < manifestLine.size())
										uriEndIndex++;
									if (manifestLine[uriEndIndex] == '\"')
									{
										string uri = manifestLine.substr(uriStartIndex, uriEndIndex - uriStartIndex);
										/*
										_logger->info(__FILEREF__ + "Creation token parameter for m3u8"
											+ ", uri: " + uri
											+ ", tokenComingFromURL: " + to_string(tokenComingFromURL)
										);
										*/
										string auth = Encrypt::encrypt(uri + "+++" + to_string(tokenComingFromURL));
										string tokenParameter = string("?token=") + auth;

										manifestLine.insert(uriEndIndex, tokenParameter);
									}
								}

								responseBody += (manifestLine + endLine);
							}
							else
							{
								responseBody += (manifestLine + endLine);
							}
						}
						manifestFile.close();
					}
					else	// dash
					{
#if defined(LIBXML_TREE_ENABLED) && defined(LIBXML_OUTPUT_ENABLED) && \
defined(LIBXML_XPATH_ENABLED) && defined(LIBXML_SAX1_ENABLED)
	_logger->info(__FILEREF__ + "libxml define OK");
#else
	_logger->info(__FILEREF__ + "libxml define KO");
#endif

/*
<?xml version="1.0" encoding="utf-8"?>
<MPD xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
        xmlns="urn:mpeg:dash:schema:mpd:2011"
        xmlns:xlink="http://www.w3.org/1999/xlink"
        xsi:schemaLocation="urn:mpeg:DASH:schema:MPD:2011 http://standards.iso.org/ittf/PubliclyAvailableStandards/MPEG-DASH_schema_files/DASH-MPD.xsd"
        profiles="urn:mpeg:dash:profile:isoff-live:2011"
        type="dynamic"
        minimumUpdatePeriod="PT10S"
        suggestedPresentationDelay="PT10S"
        availabilityStartTime="2020-02-03T15:11:56Z"
        publishTime="2020-02-04T08:54:57Z"
        timeShiftBufferDepth="PT1M0.0S"
        minBufferTime="PT20.0S">
        <ProgramInformation>
        </ProgramInformation>
        <Period id="0" start="PT0.0S">
                <AdaptationSet id="0" contentType="video" segmentAlignment="true" bitstreamSwitching="true">
                        <Representation id="0" mimeType="video/mp4" codecs="avc1.640029" bandwidth="1494920" width="1024" height="576" frameRate="25/1">
                                <SegmentTemplate timescale="12800" initialization="init-stream$RepresentationID$.m4s" media="chunk-stream$RepresentationID$-$Number%05d$.m4s" startNumber="6373">
                                        <SegmentTimeline>
                                                <S t="815616000" d="128000" r="5" />
                                        </SegmentTimeline>
                                </SegmentTemplate>
                        </Representation>
                </AdaptationSet>
                <AdaptationSet id="1" contentType="audio" segmentAlignment="true" bitstreamSwitching="true">
                        <Representation id="1" mimeType="audio/mp4" codecs="mp4a.40.5" bandwidth="95545" audioSamplingRate="48000">
                                <AudioChannelConfiguration schemeIdUri="urn:mpeg:dash:23003:3:audio_channel_configuration:2011" value="2" />
                                <SegmentTemplate timescale="48000" initialization="init-stream$RepresentationID$.m4s" media="chunk-stream$RepresentationID$-$Number%05d$.m4s" startNumber="6373">
                                        <SegmentTimeline>
                                                <S t="3058557246" d="479232" />
                                                <S d="481280" />
                                                <S d="479232" r="1" />
                                                <S d="481280" />
                                                <S d="479232" />
                                        </SegmentTimeline>
                                </SegmentTemplate>
                        </Representation>
                </AdaptationSet>
        </Period>
</MPD>
*/
						xmlDocPtr doc = xmlParseFile(manifestPathFileName.c_str());
						if (doc == NULL)
						{
							string errorMessage = string("xmlParseFile failed")
								+ ", manifestPathFileName: " + manifestPathFileName
								;
							_logger->info(__FILEREF__ + errorMessage);

							throw runtime_error(errorMessage);
						}

						// xmlNode* rootElement = xmlDocGetRootElement(doc);

						/* Create xpath evaluation context */
						xmlXPathContextPtr xpathCtx = xmlXPathNewContext(doc);
						if(xpathCtx == NULL)
						{
							xmlFreeDoc(doc);

							string errorMessage = string("xmlXPathNewContext failed")
								+ ", manifestPathFileName: " + manifestPathFileName
								;
							_logger->info(__FILEREF__ + errorMessage);

							throw runtime_error(errorMessage);
						}

						if(xmlXPathRegisterNs(xpathCtx,
							BAD_CAST "xmlns",
							BAD_CAST "urn:mpeg:dash:schema:mpd:2011") != 0)
						{
							xmlXPathFreeContext(xpathCtx);
							xmlFreeDoc(doc);

							string errorMessage = string("xmlXPathRegisterNs xmlns:xsi")
								+ ", manifestPathFileName: " + manifestPathFileName
								;
							_logger->info(__FILEREF__ + errorMessage);

							throw runtime_error(errorMessage);
						}
						/*
						if(xmlXPathRegisterNs(xpathCtx,
							BAD_CAST "xmlns:xlink",
							BAD_CAST "http://www.w3.org/1999/xlink") != 0)
						{
							xmlXPathFreeContext(xpathCtx);
							xmlFreeDoc(doc);

							string errorMessage = string("xmlXPathRegisterNs xmlns:xlink")
								+ ", manifestPathFileName: " + manifestPathFileName
								;
							_logger->info(__FILEREF__ + errorMessage);

							throw runtime_error(errorMessage);
						}
						if(xmlXPathRegisterNs(xpathCtx,
							BAD_CAST "xsi:schemaLocation",
							BAD_CAST "http://standards.iso.org/ittf/PubliclyAvailableStandards/MPEG-DASH_schema_files/DASH-MPD.xsd") != 0)
						{
							xmlXPathFreeContext(xpathCtx);
							xmlFreeDoc(doc);

							string errorMessage = string("xmlXPathRegisterNs xsi:schemaLocation")
								+ ", manifestPathFileName: " + manifestPathFileName
								;
							_logger->info(__FILEREF__ + errorMessage);

							throw runtime_error(errorMessage);
						}
						*/

						// Evaluate xpath expression
						const char *xpathExpr = "//xmlns:Period/xmlns:AdaptationSet/xmlns:Representation/xmlns:SegmentTemplate";
						xmlXPathObjectPtr xpathObj = xmlXPathEvalExpression(BAD_CAST xpathExpr, xpathCtx);
						if(xpathObj == NULL)
						{
							xmlXPathFreeContext(xpathCtx);
							xmlFreeDoc(doc);

							string errorMessage = string("xmlXPathEvalExpression failed")
								+ ", manifestPathFileName: " + manifestPathFileName
								;
							_logger->info(__FILEREF__ + errorMessage);

							throw runtime_error(errorMessage);
						}

						xmlNodeSetPtr nodes = xpathObj->nodesetval;
						_logger->info(__FILEREF__ + "processing mpd manifest file"
							+ ", manifestPathFileName: " + manifestPathFileName
							+ ", nodesNumber: " + to_string(nodes->nodeNr)
						);
						for (int nodeIndex = 0; nodeIndex < nodes->nodeNr; nodeIndex++)
						{
							if (nodes->nodeTab[nodeIndex] == NULL)
							{
								xmlXPathFreeContext(xpathCtx);
								xmlFreeDoc(doc);

								string errorMessage = string("nodes->nodeTab[nodeIndex] is null")
									+ ", manifestPathFileName: " + manifestPathFileName
									+ ", nodeIndex: " + to_string(nodeIndex)
								;
								_logger->info(__FILEREF__ + errorMessage);

								throw runtime_error(errorMessage);
							}

							const char* mediaAttributeName = "media";
							const char* initializationAttributeName = "initialization";
							xmlChar* mediaValue = xmlGetProp(nodes->nodeTab[nodeIndex],
									BAD_CAST mediaAttributeName);
							xmlChar* initializationValue = xmlGetProp(nodes->nodeTab[nodeIndex],
									BAD_CAST initializationAttributeName);
							if (mediaValue == (xmlChar*) NULL
									|| initializationValue == (xmlChar*) NULL)
							{
								xmlXPathFreeContext(xpathCtx);
								xmlFreeDoc(doc);

								string errorMessage = string("xmlGetProp failed")
									+ ", manifestPathFileName: " + manifestPathFileName
								;
								_logger->info(__FILEREF__ + errorMessage);

								throw runtime_error(errorMessage);
							}

							string auth = Encrypt::encrypt(string((char*) mediaValue)
								+ "+++" + to_string(tokenComingFromURL));
							string newMediaAttributeValue = string((char*) mediaValue)
								+ "?token=" + auth;
							// xmlAttrPtr
							xmlSetProp (nodes->nodeTab[nodeIndex],
								BAD_CAST mediaAttributeName,
								BAD_CAST newMediaAttributeValue.c_str());

							string newInitializationAttributeValue =
								string((char*) initializationValue)
								+ "?token=" + auth;
							// xmlAttrPtr
							xmlSetProp (nodes->nodeTab[nodeIndex],
								BAD_CAST initializationAttributeName,
								BAD_CAST newInitializationAttributeValue.c_str());

							// const char *value = "ssss";
							// xmlNodeSetContent(nodes->nodeTab[nodeIndex], BAD_CAST value);

							/*
							* All the elements returned by an XPath query are pointers to
							* elements from the tree *except* namespace nodes where the XPath
							* semantic is different from the implementation in libxml2 tree.
							* As a result when a returned node set is freed when
							* xmlXPathFreeObject() is called, that routine must check the
							* element type. But node from the returned set may have been removed
							* by xmlNodeSetContent() resulting in access to freed data.
							* This can be exercised by running
							*       valgrind xpath2 test3.xml '//discarded' discarded
							* There is 2 ways around it:
							*   - make a copy of the pointers to the nodes from the result set
							*     then call xmlXPathFreeObject() and then modify the nodes
							* or
							*   - remove the reference to the modified nodes from the node set
							*     as they are processed, if they are not namespace nodes.
							*/
							// if (nodes->nodeTab[nodeIndex]->type != XML_NAMESPACE_DECL)
							// 	nodes->nodeTab[nodeIndex] = NULL;
						}

						/* Cleanup of XPath data */
						xmlXPathFreeObject(xpathObj);
						xmlXPathFreeContext(xpathCtx);

						/* dump the resulting document */
						{
							xmlChar *xmlbuff;
							int buffersize;
							xmlDocDumpFormatMemoryEnc(doc, &xmlbuff, &buffersize, "UTF-8", 1);
							_logger->info(__FILEREF__ + "dumping mpd manifest file"
								+ ", manifestPathFileName: " + manifestPathFileName
								+ ", buffersize: " + to_string(buffersize)
							);

							responseBody = (char*) xmlbuff;

							xmlFree(xmlbuff);
							// xmlDocDump(stdout, doc);
						}

						/* free the document */
						xmlFreeDoc(doc);

						/*
						std::ifstream manifestFile(manifestPathFileName);
						std::stringstream buffer;
						buffer << manifestFile.rdbuf();

						responseBody = buffer.str();
						*/
					}
				}

				string cookieValue = Encrypt::encrypt(to_string(tokenComingFromURL));
				string cookiePath;
				{
					size_t cookiePathIndex = contentURI.find_last_of("/");
					if (cookiePathIndex == string::npos)
					{
						string errorMessage = string("Wrong URI format")
							+ ", contentURI: " + contentURI
							;
						_logger->info(__FILEREF__ + errorMessage);

						throw runtime_error(errorMessage);
					}
					cookiePath = contentURI.substr(0, cookiePathIndex);
				}
				bool enableCorsGETHeader = true;
				if (secondaryManifest)
					sendSuccess(request, 200, responseBody,
						contentType, "", "", "",
						enableCorsGETHeader);
				else
					sendSuccess(request, 200, responseBody,
						contentType, cookieName, cookieValue, cookiePath,
						enableCorsGETHeader);
			}
		}
        catch(runtime_error e)
        {
            string errorMessage = string("Not authorized");
            _logger->warn(__FILEREF__ + errorMessage);

			string responseBody;
			sendError(request, 403, errorMessage);
        }
        catch(exception e)
        {
            string errorMessage = string("Not authorized: exception managing token");
            _logger->warn(__FILEREF__ + errorMessage);

            sendError(request, 500, errorMessage);
        }
    }
    else if (method == "login")
    {
        login(request, requestBody);
    }
    else if (method == "registerUser")
    {
        registerUser(request, requestBody);
    }
    else if (method == "updateUser")
    {
        updateUser(request, userKey, requestBody);
    }
    else if (method == "updateWorkspace")
    {
        updateWorkspace(request, workspace, userKey, requestBody);
    }
    else if (method == "setWorkspaceAsDefault")
    {
        setWorkspaceAsDefault(request, workspace, userKey, queryParameters, requestBody);
    }
    else if (method == "createWorkspace")
    {
        if (!admin && !createRemoveWorkspace)
        {
            string errorMessage = string("APIKey does not have the permission"
                    ", createRemoveWorkspace: " + to_string(createRemoveWorkspace)
                    );
            _logger->error(__FILEREF__ + errorMessage);

            sendError(request, 403, errorMessage);

            throw runtime_error(errorMessage);
        }

        createWorkspace(request, userKey, queryParameters, requestBody, admin);
    }
    else if (method == "deleteWorkspace")
    {
        if (!admin && !createRemoveWorkspace)
        {
            string errorMessage = string("APIKey does not have the permission"
                    ", createRemoveWorkspace: " + to_string(createRemoveWorkspace)
                    );
            _logger->error(__FILEREF__ + errorMessage);

            sendError(request, 403, errorMessage);

            throw runtime_error(errorMessage);
        }

        deleteWorkspace(request, userKey, workspace);
    }
    else if (method == "workspaceUsage")
    {
        workspaceUsage(request, workspace);
    }
    else if (method == "shareWorkspace")
    {
        if (!admin && !shareWorkspace)
        {
            string errorMessage = string("APIKey does not have the permission"
                    ", shareWorkspace: " + to_string(shareWorkspace)
                    );
            _logger->error(__FILEREF__ + errorMessage);

            sendError(request, 403, errorMessage);

            throw runtime_error(errorMessage);
        }

        shareWorkspace_(request, workspace, queryParameters, requestBody);
    }
    else if (method == "workspaceList")
    {
		workspaceList(request, userKey, workspace, queryParameters, admin);
    }
    else if (method == "confirmRegistration")
    {
        confirmRegistration(request, queryParameters);
    }
    else if (method == "addEncoder")
    {
        if (!admin)
        {
            string errorMessage = string("APIKey does not have the permission"
                    ", admin: " + to_string(admin)
                    );
            _logger->error(__FILEREF__ + errorMessage);

            sendError(request, 403, errorMessage);

            throw runtime_error(errorMessage);
        }

		addEncoder(request, workspace, requestBody);
    }
    else if (method == "removeEncoder")
    {
        if (!admin)
        {
            string errorMessage = string("APIKey does not have the permission"
                    ", admin: " + to_string(admin)
                    );
            _logger->error(__FILEREF__ + errorMessage);

            sendError(request, 403, errorMessage);

            throw runtime_error(errorMessage);
        }

		removeEncoder(request, workspace, queryParameters);
    }
    else if (method == "modifyEncoder")
    {
        if (!admin)
        {
            string errorMessage = string("APIKey does not have the permission"
                    ", admin: " + to_string(admin)
                    );
            _logger->error(__FILEREF__ + errorMessage);

            sendError(request, 403, errorMessage);

            throw runtime_error(errorMessage);
        }

		modifyEncoder(request, workspace, queryParameters, requestBody);
    }
    else if (method == "encoderList")
    {
		encoderList(request, workspace, admin, queryParameters);
    }
    else if (method == "encodersPoolList")
    {
		encodersPoolList(request, workspace, admin, queryParameters);
    }
    else if (method == "addEncodersPool")
    {
        if (!admin && !editEncodersPool)
        {
            string errorMessage = string("APIKey does not have the permission"
                    ", editEncodersPool: " + to_string(editEncodersPool)
                    );
            _logger->error(__FILEREF__ + errorMessage);

            sendError(request, 403, errorMessage);

            throw runtime_error(errorMessage);
        }

		addEncodersPool(request, workspace, requestBody);
    }
    else if (method == "modifyEncodersPool")
    {
        if (!admin && !editEncodersPool)
        {
            string errorMessage = string("APIKey does not have the permission"
                    ", editEncodersPool: " + to_string(editEncodersPool)
                    );
            _logger->error(__FILEREF__ + errorMessage);

            sendError(request, 403, errorMessage);

            throw runtime_error(errorMessage);
        }

		modifyEncodersPool(request, workspace, queryParameters, requestBody);
    }
    else if (method == "removeEncodersPool")
    {
        if (!admin && !editEncodersPool)
        {
            string errorMessage = string("APIKey does not have the permission"
                    ", editEncodersPool: " + to_string(editEncodersPool)
                    );
            _logger->error(__FILEREF__ + errorMessage);

            sendError(request, 403, errorMessage);

            throw runtime_error(errorMessage);
        }

		removeEncodersPool(request, workspace, queryParameters);
    }
    else if (method == "addAssociationWorkspaceEncoder")
    {
        if (!admin)
        {
            string errorMessage = string("APIKey does not have the permission"
                    ", admin: " + to_string(admin)
                    );
            _logger->error(__FILEREF__ + errorMessage);

            sendError(request, 403, errorMessage);

            throw runtime_error(errorMessage);
        }

		addAssociationWorkspaceEncoder(request, workspace, queryParameters);
    }
    else if (method == "removeAssociationWorkspaceEncoder")
    {
        if (!admin)
        {
            string errorMessage = string("APIKey does not have the permission"
                    ", admin: " + to_string(admin)
                    );
            _logger->error(__FILEREF__ + errorMessage);

            sendError(request, 403, errorMessage);

            throw runtime_error(errorMessage);
        }

		removeAssociationWorkspaceEncoder(request, workspace, queryParameters);
    }
    else if (method == "createDeliveryAuthorization")
    {
        if (!admin && !deliveryAuthorization)
        {
            string errorMessage = string("APIKey does not have the permission"
                    ", deliveryAuthorization: " + to_string(deliveryAuthorization)
                    );
            _logger->error(__FILEREF__ + errorMessage);

            sendError(request, 403, errorMessage);

            throw runtime_error(errorMessage);
        }

        string clientIPAddress;
        auto remoteAddrIt = requestDetails.find("REMOTE_ADDR");
        if (remoteAddrIt != requestDetails.end())
            clientIPAddress = remoteAddrIt->second;

		createDeliveryAuthorization(request, userKey, workspace,
			clientIPAddress, queryParameters);
    }
    else if (method == "createDeliveryCDN77Authorization")
    {
        if (!admin && !deliveryAuthorization)
        {
            string errorMessage = string("APIKey does not have the permission"
                    ", deliveryAuthorization: " + to_string(deliveryAuthorization)
                    );
            _logger->error(__FILEREF__ + errorMessage);

            sendError(request, 403, errorMessage);

            throw runtime_error(errorMessage);
        }

        string clientIPAddress;
        auto remoteAddrIt = requestDetails.find("REMOTE_ADDR");
        if (remoteAddrIt != requestDetails.end())
            clientIPAddress = remoteAddrIt->second;

        createDeliveryCDN77Authorization(request, userKey, workspace,
                clientIPAddress, queryParameters);
    }
    else if (method == "ingestion")
    {
        if (!admin && !ingestWorkflow)
        {
            string errorMessage = string("APIKey does not have the permission"
                    ", ingestWorkflow: " + to_string(ingestWorkflow)
                    );
            _logger->error(__FILEREF__ + errorMessage);

            sendError(request, 403, errorMessage);

            throw runtime_error(errorMessage);
        }
        
        ingestion(request, userKey, apiKey, workspace, queryParameters, requestBody);
    }
    else if (method == "ingestionRootsStatus")
    {
        ingestionRootsStatus(request, workspace, queryParameters, requestBody);
    }
    else if (method == "ingestionRootMetaDataContent")
    {
        ingestionRootMetaDataContent(request, workspace, queryParameters, requestBody);
    }
    else if (method == "ingestionJobsStatus")
    {
        ingestionJobsStatus(request, workspace, queryParameters, requestBody);
    }
    else if (method == "cancelIngestionJob")
    {
        if (!admin && !cancelIngestionJob_)
        {
            string errorMessage = string("APIKey does not have the permission"
                    ", cancelIngestionJob: " + to_string(cancelIngestionJob_)
                    );
            _logger->error(__FILEREF__ + errorMessage);

            sendError(request, 403, errorMessage);

            throw runtime_error(errorMessage);
        }

        cancelIngestionJob(request, workspace, queryParameters, requestBody);
    }
    else if (method == "updateIngestionJob")
    {
        if (!admin && !editMedia)
        {
            string errorMessage = string("APIKey does not have the permission"
                    ", editMedia: " + to_string(editMedia)
                    );
            _logger->error(__FILEREF__ + errorMessage);

            sendError(request, 403, errorMessage);

            throw runtime_error(errorMessage);
        }

        updateIngestionJob(request, workspace, userKey, queryParameters, requestBody, admin);
    }
    else if (method == "encodingJobsStatus")
    {
        encodingJobsStatus(request, workspace, queryParameters, requestBody);
    }
    else if (method == "encodingJobPriority")
    {
        encodingJobPriority(request, workspace, queryParameters, requestBody);
    }
    else if (method == "killOrCancelEncodingJob")
    {
        if (!admin && !killEncoding)
        {
            string errorMessage = string("APIKey does not have the permission"
                    ", killEncoding: " + to_string(killEncoding)
                    );
            _logger->error(__FILEREF__ + errorMessage);

            sendError(request, 403, errorMessage);

            throw runtime_error(errorMessage);
        }

        killOrCancelEncodingJob(request, workspace, queryParameters, requestBody);
    }
    else if (method == "mediaItemsList")
    {
        mediaItemsList(request, workspace, queryParameters, requestBody, admin);
    }
    else if (method == "updateMediaItem")
    {
        if (!admin && !editMedia)
        {
            string errorMessage = string("APIKey does not have the permission"
                    ", editMedia: " + to_string(editMedia)
                    );
            _logger->error(__FILEREF__ + errorMessage);

            sendError(request, 403, errorMessage);

            throw runtime_error(errorMessage);
        }
        
        updateMediaItem(request, workspace, userKey, queryParameters, requestBody, admin);
    }
    else if (method == "updatePhysicalPath")
    {
        if (!admin && !editMedia)
        {
            string errorMessage = string("APIKey does not have the permission"
                    ", editMedia: " + to_string(editMedia)
                    );
            _logger->error(__FILEREF__ + errorMessage);

            sendError(request, 403, errorMessage);

            throw runtime_error(errorMessage);
        }
        
        updatePhysicalPath(request, workspace, userKey, queryParameters, requestBody, admin);
    }
    else if (method == "tagsList")
    {
        tagsList(request, workspace, queryParameters, requestBody);
    }
    else if (method == "uploadedBinary")
    {
        uploadedBinary(request, requestMethod,
            queryParameters, workspace, // contentLength,
                requestDetails);
    }
    else if (method == "addEncodingProfilesSet")
    {
        if (!admin && !createProfiles)
        {
            string errorMessage = string("APIKey does not have the permission"
                    ", createProfiles: " + to_string(createProfiles)
                    );
            _logger->error(__FILEREF__ + errorMessage);

            sendError(request, 403, errorMessage);

            throw runtime_error(errorMessage);
        }

        addEncodingProfilesSet(request, workspace,
            queryParameters, requestBody);
    }
    else if (method == "encodingProfilesSetsList")
    {
        encodingProfilesSetsList(request, workspace, queryParameters);
    }
    else if (method == "addEncodingProfile")
    {
        if (!admin && !createProfiles)
        {
            string errorMessage = string("APIKey does not have the permission"
                    ", createProfiles: " + to_string(createProfiles)
                    );
            _logger->error(__FILEREF__ + errorMessage);

            sendError(request, 403, errorMessage);

            throw runtime_error(errorMessage);
        }
                
        addEncodingProfile(request, workspace,
            queryParameters, requestBody);
    }
    else if (method == "removeEncodingProfile")
    {
        if (!admin && !createProfiles)
        {
            string errorMessage = string("APIKey does not have the permission"
                    ", createProfiles: " + to_string(createProfiles)
                    );
            _logger->error(__FILEREF__ + errorMessage);

            sendError(request, 403, errorMessage);

            throw runtime_error(errorMessage);
        }
                
        removeEncodingProfile(request, workspace,
            queryParameters);
    }
    else if (method == "removeEncodingProfilesSet")
    {
        if (!admin && !createProfiles)
        {
            string errorMessage = string("APIKey does not have the permission"
                    ", createProfiles: " + to_string(createProfiles)
                    );
            _logger->error(__FILEREF__ + errorMessage);

            sendError(request, 403, errorMessage);

            throw runtime_error(errorMessage);
        }

        removeEncodingProfilesSet(request, workspace,
            queryParameters);
    }
    else if (method == "encodingProfilesList")
    {
        encodingProfilesList(request, workspace, queryParameters);
    }
    else if (method == "workflowsAsLibraryList")
    {
        workflowsAsLibraryList(request, workspace, queryParameters);
    }
    else if (method == "workflowAsLibraryContent")
    {
        workflowAsLibraryContent(request, workspace, queryParameters);
    }
    else if (method == "saveWorkflowAsLibrary")
    {
        saveWorkflowAsLibrary(request, userKey, workspace, queryParameters, requestBody, admin);
    }
    else if (method == "removeWorkflowAsLibrary")
    {
        removeWorkflowAsLibrary(request, userKey, workspace, queryParameters, admin);
    }
    else if (method == "mmsSupport")
    {
		mmsSupport(request, userKey, apiKey, workspace, queryParameters, requestBody);
    }
    else if (method == "addYouTubeConf")
    {
        if (!admin && !editConfiguration)
        {
            string errorMessage = string("APIKey does not have the permission"
                    ", editConfiguration: " + to_string(editConfiguration)
                    );
            _logger->error(__FILEREF__ + errorMessage);

            sendError(request, 403, errorMessage);

            throw runtime_error(errorMessage);
        }

        addYouTubeConf(request, workspace, queryParameters, requestBody);
    }
    else if (method == "modifyYouTubeConf")
    {
        if (!admin && !editConfiguration)
        {
            string errorMessage = string("APIKey does not have the permission"
                    ", editConfiguration: " + to_string(editConfiguration)
                    );
            _logger->error(__FILEREF__ + errorMessage);

            sendError(request, 403, errorMessage);

            throw runtime_error(errorMessage);
        }

        modifyYouTubeConf(request, workspace, queryParameters, requestBody);
    }
    else if (method == "removeYouTubeConf")
    {
        if (!admin && !editConfiguration)
        {
            string errorMessage = string("APIKey does not have the permission"
                    ", editConfiguration: " + to_string(editConfiguration)
                    );
            _logger->error(__FILEREF__ + errorMessage);

            sendError(request, 403, errorMessage);

            throw runtime_error(errorMessage);
        }

        removeYouTubeConf(request, workspace, queryParameters);
    }
    else if (method == "youTubeConfList")
    {
        youTubeConfList(request, workspace);
    }
    else if (method == "addFacebookConf")
    {
        if (!admin && !editConfiguration)
        {
            string errorMessage = string("APIKey does not have the permission"
                    ", editConfiguration: " + to_string(editConfiguration)
                    );
            _logger->error(__FILEREF__ + errorMessage);

            sendError(request, 403, errorMessage);

            throw runtime_error(errorMessage);
        }

        addFacebookConf(request, workspace, queryParameters, requestBody);
    }
    else if (method == "modifyFacebookConf")
    {
        if (!admin && !editConfiguration)
        {
            string errorMessage = string("APIKey does not have the permission"
                    ", editConfiguration: " + to_string(editConfiguration)
                    );
            _logger->error(__FILEREF__ + errorMessage);

            sendError(request, 403, errorMessage);

            throw runtime_error(errorMessage);
        }

        modifyFacebookConf(request, workspace, queryParameters, requestBody);
    }
    else if (method == "removeFacebookConf")
    {
        if (!admin && !editConfiguration)
        {
            string errorMessage = string("APIKey does not have the permission"
                    ", editConfiguration: " + to_string(editConfiguration)
                    );
            _logger->error(__FILEREF__ + errorMessage);

            sendError(request, 403, errorMessage);

            throw runtime_error(errorMessage);
        }

        removeFacebookConf(request, workspace, queryParameters);
    }
    else if (method == "facebookConfList")
    {
        facebookConfList(request, workspace);
    }
    else if (method == "addIPChannelConf")
    {
        if (!admin && !editConfiguration)
        {
            string errorMessage = string("APIKey does not have the permission"
                    ", editConfiguration: " + to_string(editConfiguration)
                    );
            _logger->error(__FILEREF__ + errorMessage);

            sendError(request, 403, errorMessage);

            throw runtime_error(errorMessage);
        }

        addIPChannelConf(request, workspace, queryParameters, requestBody);
    }
    else if (method == "modifyIPChannelConf")
    {
        if (!admin && !editConfiguration)
        {
            string errorMessage = string("APIKey does not have the permission"
                    ", editConfiguration: " + to_string(editConfiguration)
                    );
            _logger->error(__FILEREF__ + errorMessage);

            sendError(request, 403, errorMessage);

            throw runtime_error(errorMessage);
        }

        modifyIPChannelConf(request, workspace, queryParameters, requestBody);
    }
    else if (method == "removeIPChannelConf")
    {
        if (!admin && !editConfiguration)
        {
            string errorMessage = string("APIKey does not have the permission"
                    ", editConfiguration: " + to_string(editConfiguration)
                    );
            _logger->error(__FILEREF__ + errorMessage);

            sendError(request, 403, errorMessage);

            throw runtime_error(errorMessage);
        }

        removeIPChannelConf(request, workspace, queryParameters);
    }
    else if (method == "ipChannelConfList")
    {
        ipChannelConfList(request, workspace, queryParameters);
    }
    else if (method == "addSATChannelConf")
    {
        if (!admin && !editConfiguration)
        {
            string errorMessage = string("APIKey does not have the permission"
                    ", editConfiguration: " + to_string(editConfiguration)
                    );
            _logger->error(__FILEREF__ + errorMessage);

            sendError(request, 403, errorMessage);

            throw runtime_error(errorMessage);
        }

        addSATChannelConf(request, workspace, queryParameters, requestBody);
    }
    else if (method == "modifySATChannelConf")
    {
        if (!admin && !editConfiguration)
        {
            string errorMessage = string("APIKey does not have the permission"
                    ", editConfiguration: " + to_string(editConfiguration)
                    );
            _logger->error(__FILEREF__ + errorMessage);

            sendError(request, 403, errorMessage);

            throw runtime_error(errorMessage);
        }

        modifySATChannelConf(request, workspace, queryParameters, requestBody);
    }
    else if (method == "removeSATChannelConf")
    {
        if (!admin && !editConfiguration)
        {
            string errorMessage = string("APIKey does not have the permission"
                    ", editConfiguration: " + to_string(editConfiguration)
                    );
            _logger->error(__FILEREF__ + errorMessage);

            sendError(request, 403, errorMessage);

            throw runtime_error(errorMessage);
        }

        removeSATChannelConf(request, workspace, queryParameters);
    }
    else if (method == "satChannelConfList")
    {
        satChannelConfList(request, workspace, queryParameters);
    }
    else if (method == "addSourceSATChannelConf")
    {
        if (!admin)
        {
            string errorMessage = string("APIKey does not have the permission"
                    ", admin: " + to_string(admin)
                    );
            _logger->error(__FILEREF__ + errorMessage);

            sendError(request, 403, errorMessage);

            throw runtime_error(errorMessage);
        }

        addSourceSATChannelConf(request, workspace, queryParameters, requestBody);
    }
    else if (method == "modifySourceSATChannelConf")
    {
        if (!admin)
        {
            string errorMessage = string("APIKey does not have the permission"
                    ", admin: " + to_string(admin)
                    );
            _logger->error(__FILEREF__ + errorMessage);

            sendError(request, 403, errorMessage);

            throw runtime_error(errorMessage);
        }

        modifySourceSATChannelConf(request, workspace, queryParameters, requestBody);
    }
    else if (method == "removeSourceSATChannelConf")
    {
        if (!admin)
        {
            string errorMessage = string("APIKey does not have the permission"
                    ", admin: " + to_string(admin)
                    );
            _logger->error(__FILEREF__ + errorMessage);

            sendError(request, 403, errorMessage);

            throw runtime_error(errorMessage);
        }

        removeSourceSATChannelConf(request, workspace, queryParameters);
    }
    else if (method == "sourceSatChannelConfList")
    {
        sourceSatChannelConfList(request, workspace, queryParameters);
    }
    else if (method == "addFTPConf")
    {
        if (!admin && !editConfiguration)
        {
            string errorMessage = string("APIKey does not have the permission"
                    ", editConfiguration: " + to_string(editConfiguration)
                    );
            _logger->error(__FILEREF__ + errorMessage);

            sendError(request, 403, errorMessage);

            throw runtime_error(errorMessage);
        }

        addFTPConf(request, workspace, queryParameters, requestBody);
    }
    else if (method == "modifyFTPConf")
    {
        if (!admin && !editConfiguration)
        {
            string errorMessage = string("APIKey does not have the permission"
                    ", editConfiguration: " + to_string(editConfiguration)
                    );
            _logger->error(__FILEREF__ + errorMessage);

            sendError(request, 403, errorMessage);

            throw runtime_error(errorMessage);
        }

        modifyFTPConf(request, workspace, queryParameters, requestBody);
    }
    else if (method == "removeFTPConf")
    {
        if (!admin && !editConfiguration)
        {
            string errorMessage = string("APIKey does not have the permission"
                    ", editConfiguration: " + to_string(editConfiguration)
                    );
            _logger->error(__FILEREF__ + errorMessage);

            sendError(request, 403, errorMessage);

            throw runtime_error(errorMessage);
        }

        removeFTPConf(request, workspace, queryParameters);
    }
    else if (method == "ftpConfList")
    {
        ftpConfList(request, workspace);
    }
    else if (method == "addEMailConf")
    {
        if (!admin && !editConfiguration)
        {
            string errorMessage = string("APIKey does not have the permission"
                    ", editConfiguration: " + to_string(editConfiguration)
                    );
            _logger->error(__FILEREF__ + errorMessage);

            sendError(request, 403, errorMessage);

            throw runtime_error(errorMessage);
        }

        addEMailConf(request, workspace, queryParameters, requestBody);
    }
    else if (method == "modifyEMailConf")
    {
        if (!admin && !editConfiguration)
        {
            string errorMessage = string("APIKey does not have the permission"
                    ", editConfiguration: " + to_string(editConfiguration)
                    );
            _logger->error(__FILEREF__ + errorMessage);

            sendError(request, 403, errorMessage);

            throw runtime_error(errorMessage);
        }

        modifyEMailConf(request, workspace, queryParameters, requestBody);
    }
    else if (method == "removeEMailConf")
    {
        if (!admin && !editConfiguration)
        {
            string errorMessage = string("APIKey does not have the permission"
                    ", editConfiguration: " + to_string(editConfiguration)
                    );
            _logger->error(__FILEREF__ + errorMessage);

            sendError(request, 403, errorMessage);

            throw runtime_error(errorMessage);
        }

        removeEMailConf(request, workspace, queryParameters);
    }
    else if (method == "emailConfList")
    {
        emailConfList(request, workspace);
    }
    else
    {
        string errorMessage = string("No API is matched")
            + ", requestURI: " + requestURI
            + ", method: " + method
            + ", requestMethod: " + requestMethod
                ;
        _logger->error(__FILEREF__ + errorMessage);

        sendError(request, 400, errorMessage);

        throw runtime_error(errorMessage);
    }
}

void API::createDeliveryAuthorization(
        FCGX_Request& request,
        int64_t userKey,
        shared_ptr<Workspace> requestWorkspace,
        string clientIPAddress,
        unordered_map<string, string> queryParameters)
{
    string api = "createDeliveryAuthorization";

    _logger->info(__FILEREF__ + "Received " + api
    );

    try
    {
        int64_t physicalPathKey = -1;
        auto physicalPathKeyIt = queryParameters.find("physicalPathKey");
        if (physicalPathKeyIt != queryParameters.end())
        {
			physicalPathKey = stoll(physicalPathKeyIt->second);
        }

        int64_t mediaItemKey = -1;
        auto mediaItemKeyIt = queryParameters.find("mediaItemKey");
        if (mediaItemKeyIt != queryParameters.end())
        {
			mediaItemKey = stoll(mediaItemKeyIt->second);
			if (mediaItemKey == 0)
				mediaItemKey = -1;
        }

        string uniqueName;
        auto uniqueNameIt = queryParameters.find("uniqueName");
        if (uniqueNameIt != queryParameters.end())
        {
			uniqueName = uniqueNameIt->second;

			// 2021-01-07: Remark: we have FIRST to replace + in space and then apply curlpp::unescape
			//	That  because if we have really a + char (%2B into the string), and we do the replace
			//	after curlpp::unescape, this char will be changed to space and we do not want it
			string plus = "\\+";
			string plusDecoded = " ";
			string firstDecoding = regex_replace(uniqueName, regex(plus), plusDecoded);

			uniqueName = curlpp::unescape(firstDecoding);
        }

        int64_t encodingProfileKey = -1;
        auto encodingProfileKeyIt = queryParameters.find("encodingProfileKey");
        if (encodingProfileKeyIt != queryParameters.end())
		{
			encodingProfileKey = stoll(encodingProfileKeyIt->second);
			if (encodingProfileKey == 0)
				encodingProfileKey = -1;
		}

        string encodingProfileLabel;
        auto encodingProfileLabelIt = queryParameters.find("encodingProfileLabel");
        if (encodingProfileLabelIt != queryParameters.end())
        {
			encodingProfileLabel = encodingProfileLabelIt->second;

			// 2021-01-07: Remark: we have FIRST to replace + in space and then apply curlpp::unescape
			//	That  because if we have really a + char (%2B into the string), and we do the replace
			//	after curlpp::unescape, this char will be changed to space and we do not want it
			string plus = "\\+";
			string plusDecoded = " ";
			string firstDecoding = regex_replace(encodingProfileLabel, regex(plus), plusDecoded);

			encodingProfileLabel = curlpp::unescape(firstDecoding);
        }

		// this is for live authorization
        int64_t ingestionJobKey = -1;
        auto ingestionJobKeyIt = queryParameters.find("ingestionJobKey");
        if (ingestionJobKeyIt != queryParameters.end())
        {
			ingestionJobKey = stoll(ingestionJobKeyIt->second);
        }

		// this is for live authorization
        int64_t deliveryCode = -1;
        auto deliveryCodeIt = queryParameters.find("deliveryCode");
        if (deliveryCodeIt != queryParameters.end())
        {
			deliveryCode = stoll(deliveryCodeIt->second);
        }

		if (physicalPathKey == -1
			&& ((mediaItemKey == -1 && uniqueName == "") || (encodingProfileKey == -1 && encodingProfileLabel == ""))
			&& ingestionJobKey == -1)
		{
            string errorMessage = string("The 'physicalPathKey' or the (mediaItemKey-uniqueName)/(encodingProfileKey-encodingProfileLabel) or ingestionJobKey parameters have to be present");
            _logger->error(__FILEREF__ + errorMessage);

            sendError(request, 400, errorMessage);

            throw runtime_error(errorMessage);
		}

        int ttlInSeconds = _defaultTTLInSeconds;
        auto ttlInSecondsIt = queryParameters.find("ttlInSeconds");
        if (ttlInSecondsIt != queryParameters.end() && ttlInSecondsIt->second != "")
        {
            ttlInSeconds = stol(ttlInSecondsIt->second);
        }

        int maxRetries = _defaultMaxRetries;
        auto maxRetriesIt = queryParameters.find("maxRetries");
        if (maxRetriesIt != queryParameters.end() && maxRetriesIt->second != "")
        {
            maxRetries = stol(maxRetriesIt->second);
        }
        
        bool redirect = _defaultRedirect;
        auto redirectIt = queryParameters.find("redirect");
        if (redirectIt != queryParameters.end())
        {
            if (redirectIt->second == "true")
                redirect = true;
            else
                redirect = false;
        }
        
        bool save = false;
        auto saveIt = queryParameters.find("save");
        if (saveIt != queryParameters.end())
        {
            if (saveIt->second == "true")
                save = true;
            else
                save = false;
        }

        bool authorizationThroughPath = false;
        auto authorizationThroughPathIt = queryParameters.find("authorizationThroughPath");
        if (authorizationThroughPathIt != queryParameters.end())
        {
            if (authorizationThroughPathIt->second == "true")
                authorizationThroughPath = true;
            else
                authorizationThroughPath = false;
        }

        try
        {
			string deliveryURL;
			string deliveryFileName;

			if (ingestionJobKey == -1)
			{
				string deliveryURI;

				if (physicalPathKey != -1)
				{
					pair<string, string> deliveryFileNameAndDeliveryURI =
						_mmsStorage->getVODDeliveryURI(_mmsEngineDBFacade, physicalPathKey, save, requestWorkspace);

					tie(deliveryFileName, deliveryURI) = deliveryFileNameAndDeliveryURI;
				}
				else
				{
					if (uniqueName != "" && mediaItemKey == -1)
					{
						// initialize mediaItemKey

						bool warningIfMissing = false;
						pair<int64_t, MMSEngineDBFacade::ContentType> mediaItemKeyDetails =
							_mmsEngineDBFacade->getMediaItemKeyDetailsByUniqueName(
								requestWorkspace->_workspaceKey,
								uniqueName, warningIfMissing);
						tie(mediaItemKey, ignore) = mediaItemKeyDetails;
					}

					if (encodingProfileKey == -1 && encodingProfileLabel != "")
					{
						// initialize encodingProfileKey

						MMSEngineDBFacade::ContentType contentType;

						tuple<MMSEngineDBFacade::ContentType, string, string, string,
							int64_t, int64_t> mediaItemDetails;
						mediaItemDetails = _mmsEngineDBFacade->getMediaItemKeyDetails(
							requestWorkspace->_workspaceKey, mediaItemKey,
							false	// warningIfMissing
						);
						tie(contentType, ignore, ignore, ignore, ignore, ignore) =
							mediaItemDetails;

						encodingProfileKey = _mmsEngineDBFacade->getEncodingProfileKeyByLabel(
							requestWorkspace->_workspaceKey,
							contentType,
							encodingProfileLabel,
							true	// contentTypeToBeUsed
						);
					}

					tuple<int64_t, string, string> physicalPathKeyDeliveryFileNameAndDeliveryURI =
						_mmsStorage->getVODDeliveryURI(_mmsEngineDBFacade, mediaItemKey,
							encodingProfileKey, save, requestWorkspace);
					tie(physicalPathKey, deliveryFileName, deliveryURI) =
						physicalPathKeyDeliveryFileNameAndDeliveryURI;
				}

				if (authorizationThroughPath)
				{
					time_t expirationTime = chrono::system_clock::to_time_t(chrono::system_clock::now());
					expirationTime += ttlInSeconds;

					string uriToBeSigned;
					{
						string m3u8Suffix(".m3u8");
						if (deliveryURI.size() >= m3u8Suffix.size()
							&& 0 == deliveryURI.compare(deliveryURI.size()-m3u8Suffix.size(), m3u8Suffix.size(), m3u8Suffix))
						{
							size_t endPathIndex = deliveryURI.find_last_of("/");
							if (endPathIndex == string::npos)
								uriToBeSigned = deliveryURI;
							else
								uriToBeSigned = deliveryURI.substr(0, endPathIndex);
						}
						else
							uriToBeSigned = deliveryURI;
					}
					string md5Base64 = getSignedPath(uriToBeSigned, expirationTime);

					deliveryURL = 
						_deliveryProtocol
						+ "://" 
						+ _deliveryHost_authorizationThroughPath
						+ "/token_" + md5Base64 + "," + to_string(expirationTime)
						+ deliveryURI
					;
				}
				else
				{
					int64_t authorizationKey = _mmsEngineDBFacade->createDeliveryAuthorization(
						userKey,
						clientIPAddress,
						physicalPathKey,
						-1,
						deliveryURI,
						ttlInSeconds,
						maxRetries);

					deliveryURL = 
						_deliveryProtocol
						+ "://" 
						+ _deliveryHost_authorizationThroughParameter
						+ deliveryURI
						+ "?token=" + to_string(authorizationKey)
					;

					if (save && deliveryFileName != "")
						deliveryURL.append("&deliveryFileName=").append(deliveryFileName);
				}
			}
			else
			{
				// create authorization for a live request

				tuple<string, MMSEngineDBFacade::IngestionType, MMSEngineDBFacade::IngestionStatus, string, string>
					ingestionJobDetails = _mmsEngineDBFacade->getIngestionJobDetails(
							requestWorkspace->_workspaceKey, ingestionJobKey);
				MMSEngineDBFacade::IngestionType ingestionType;
				string metaDataContent;
				tie(ignore, ingestionType, ignore, metaDataContent, ignore) = ingestionJobDetails;

				if (ingestionType != MMSEngineDBFacade::IngestionType::LiveProxy
					&& ingestionType != MMSEngineDBFacade::IngestionType::LiveGrid
					&& ingestionType != MMSEngineDBFacade::IngestionType::LiveRecorder
					&& ingestionType != MMSEngineDBFacade::IngestionType::AwaitingTheBeginning
				)
				{
					string errorMessage = string("ingestionJob is not a LiveProxy")
						+ ", ingestionType: " + MMSEngineDBFacade::toString(ingestionType)
					;
					_logger->error(__FILEREF__ + errorMessage);

					throw runtime_error(errorMessage);
				}

				Json::Value ingestionJobRoot;
				string errors;
				try
				{
					Json::CharReaderBuilder builder;
					Json::CharReader* reader = builder.newCharReader();

					bool parsingSuccessful = reader->parse(metaDataContent.c_str(),
						metaDataContent.c_str() + metaDataContent.size(), 
						&ingestionJobRoot, &errors);
					delete reader;

					if (!parsingSuccessful)
					{
						string errorMessage = string("metadata ingestionJob parsing failed")
							+ ", errors: " + errors
							+ ", json metaDataContent: " + metaDataContent
                        ;
						_logger->error(__FILEREF__ + errorMessage);

						throw runtime_error(errorMessage);
					}
				}
				catch(exception e)
				{
					string errorMessage = string("metadata ingestionJob parsing failed")
						+ ", errors: " + errors
						+ ", json metaDataContent: " + metaDataContent
					;
					_logger->error(__FILEREF__ + errorMessage);

					throw runtime_error(errorMessage);
				}

				if (ingestionType == MMSEngineDBFacade::IngestionType::LiveProxy)
				{
					string field = "ChannelType";
					string channelType = ingestionJobRoot.get(field, "IP_MMSAsClient").asString();

					field = "Outputs";
					if (!JSONUtils::isMetadataPresent(ingestionJobRoot, field))
					{
						string errorMessage = string("A Live-Proxy without Outputs cannot be delivered")
							+ ", ingestionJobKey: " + to_string(ingestionJobKey)
						;
						_logger->error(__FILEREF__ + errorMessage);

						throw runtime_error(errorMessage);
					}
					Json::Value outputsRoot = ingestionJobRoot[field];

					vector<pair<int64_t, string>> outputsDeliveryCodesAndOutputTypes;
					{
						for (int outputIndex = 0; outputIndex < outputsRoot.size(); outputIndex++)
						{
							Json::Value outputRoot = outputsRoot[outputIndex];

							string outputType;

							field = "OutputType";
							if (!JSONUtils::isMetadataPresent(outputRoot, field))
								outputType = "HLS";
							else
								outputType = outputRoot.get(field, "HLS").asString();

							if (outputType == "HLS" || outputType == "DASH")
							{
								field = "DeliveryCode";
								if (JSONUtils::isMetadataPresent(outputRoot, field))
								{
									int64_t localDeliveryCode = outputRoot.get(field, 0).asInt64();

									outputsDeliveryCodesAndOutputTypes.push_back(
										make_pair(localDeliveryCode, outputType));
								}
							}
						}
					}

					string outputType = "HLS";	// default HLS

					if (deliveryCode == -1)	// requested delivery code (it is an input)
					{
						if (outputsDeliveryCodesAndOutputTypes.size() == 0)
						{
							if (channelType == "IP_MMSAsClient")
							{
								_logger->info(__FILEREF__ + "This is just a message to understand if the code pass from here or we can remove this condition I added because of cibor");
								// deliveryCode shall be mandatory. Just for backward compatibility
								// (cibor project), in case it is missing and it is IP_MMSAsClient, we set it
								// with confKey

								field = "ConfigurationLabel";
								string configurationLabel = ingestionJobRoot.get(field, "").asString();

								int64_t liveURLConfKey;
								bool warningIfMissing = false;
								pair<int64_t, string> liveURLConfDetails = _mmsEngineDBFacade->getIPChannelConfDetails(
									requestWorkspace->_workspaceKey, configurationLabel, warningIfMissing);
								tie(liveURLConfKey, ignore) = liveURLConfDetails;

								deliveryCode = liveURLConfKey;
							}
							else
							{
								string errorMessage = string("Live authorization without DeliveryCode cannot be delivered")
									+ ", ingestionJobKey: " + to_string(ingestionJobKey)
								;
								_logger->error(__FILEREF__ + errorMessage);

								throw runtime_error(errorMessage);
							}
						}
						else if (outputsDeliveryCodesAndOutputTypes.size() > 1)
						{
							string errorMessage = string("Live authorization with several delivery code. Just get the first")
								+ ", ingestionJobKey: " + to_string(ingestionJobKey)
							;
							_logger->warn(__FILEREF__ + errorMessage);

							deliveryCode = outputsDeliveryCodesAndOutputTypes[0].first;
							outputType = outputsDeliveryCodesAndOutputTypes[0].second;
						}
						else
						{
							// we have just one delivery code, it will be this one
							deliveryCode = outputsDeliveryCodesAndOutputTypes[0].first;
							outputType = outputsDeliveryCodesAndOutputTypes[0].second;

						}
					}
					else
					{
						bool deliveryCodeFound = false;

						for (pair<int64_t, string> deliveryCodeAndOutputType: outputsDeliveryCodesAndOutputTypes)
						{
							if (deliveryCodeAndOutputType.first == deliveryCode)
							{
								deliveryCodeFound = true;

								outputType = deliveryCodeAndOutputType.second;

								break;
							}
						}

						if (!deliveryCodeFound)
						{
							string errorMessage = string("DeliveryCode received does not exist for the ingestionJob")
								+ ", ingestionJobKey: " + to_string(ingestionJobKey)
							;
							_logger->error(__FILEREF__ + errorMessage);

							throw runtime_error(errorMessage);
						}
					}

					string deliveryURI;
					string liveFileExtension;
					if (outputType == "HLS")
						liveFileExtension = "m3u8";
					else
						liveFileExtension = "mpd";

					tuple<string, string, string> liveDeliveryDetails
						= _mmsStorage->getLiveDeliveryDetails(
						_mmsEngineDBFacade, to_string(deliveryCode),
						liveFileExtension, requestWorkspace);
					tie(deliveryURI, ignore, deliveryFileName) =
						liveDeliveryDetails;

					if (authorizationThroughPath)
					{
						time_t expirationTime = chrono::system_clock::to_time_t(chrono::system_clock::now());
						expirationTime += ttlInSeconds;

						string uriToBeSigned;
						{
							string m3u8Suffix(".m3u8");
							if (deliveryURI.size() >= m3u8Suffix.size()
								&& 0 == deliveryURI.compare(deliveryURI.size()-m3u8Suffix.size(), m3u8Suffix.size(), m3u8Suffix))
							{
								size_t endPathIndex = deliveryURI.find_last_of("/");
								if (endPathIndex == string::npos)
									uriToBeSigned = deliveryURI;
								else
									uriToBeSigned = deliveryURI.substr(0, endPathIndex);
							}
							else
								uriToBeSigned = deliveryURI;
						}
						string md5Base64 = getSignedPath(uriToBeSigned, expirationTime);

						deliveryURL = 
							_deliveryProtocol
							+ "://" 
							+ _deliveryHost_authorizationThroughPath
							+ "/token_" + md5Base64 + "," + to_string(expirationTime)
							+ deliveryURI
						;
					}
					else
					{
						int64_t authorizationKey = _mmsEngineDBFacade->createDeliveryAuthorization(
							userKey,
							clientIPAddress,
							-1,	// physicalPathKey,	vod key
							deliveryCode,		// live key
							deliveryURI,
							ttlInSeconds,
							maxRetries);

						deliveryURL = 
							_deliveryProtocol
							+ "://" 
							+ _deliveryHost_authorizationThroughParameter
							+ deliveryURI
							+ "?token=" + to_string(authorizationKey)
						;
					}
				}
				else if (ingestionType == MMSEngineDBFacade::IngestionType::LiveRecorder)
				{
					bool monitorHLS = false;
					
					string field = "MonitorHLS";
					if (JSONUtils::isMetadataPresent(ingestionJobRoot, field))
						monitorHLS = true;

					vector<pair<int64_t, string>> outputsDeliveryCodesAndOutputTypes;
					field = "Outputs";
					if (JSONUtils::isMetadataPresent(ingestionJobRoot, field))
					{
						Json::Value outputsRoot = ingestionJobRoot[field];
						for (int outputIndex = 0; outputIndex < outputsRoot.size(); outputIndex++)
						{
							Json::Value outputRoot = outputsRoot[outputIndex];

							string outputType;

							field = "OutputType";
							if (!JSONUtils::isMetadataPresent(outputRoot, field))
								outputType = "HLS";
							else
								outputType = outputRoot.get(field, "HLS").asString();

							if (outputType == "HLS" || outputType == "DASH")
							{
								field = "DeliveryCode";
								if (JSONUtils::isMetadataPresent(outputRoot, field))
								{
									int64_t localDeliveryCode = outputRoot.get(field, 0).asInt64();

									outputsDeliveryCodesAndOutputTypes.push_back(
										make_pair(localDeliveryCode, outputType));
								}
							}
						}
					}

					string outputType = "HLS";	// default HLS

					if (!monitorHLS && deliveryCode == -1)
					{
						// no monitorHLS and no input delivery code

						if (outputsDeliveryCodesAndOutputTypes.size() == 0)
						{
							string errorMessage = string("Live authorization without DeliveryCode cannot be delivered")
								+ ", ingestionJobKey: " + to_string(ingestionJobKey)
							;
							_logger->error(__FILEREF__ + errorMessage);

							throw runtime_error(errorMessage);
						}
						else if (outputsDeliveryCodesAndOutputTypes.size() > 1)
						{
							string errorMessage = string("Live authorization with several delivery code. Just get the first")
								+ ", ingestionJobKey: " + to_string(ingestionJobKey)
							;
							_logger->warn(__FILEREF__ + errorMessage);

							deliveryCode = outputsDeliveryCodesAndOutputTypes[0].first;
							outputType = outputsDeliveryCodesAndOutputTypes[0].second;
						}
						else
						{
							// we have just one delivery code, it will be this one
							deliveryCode = outputsDeliveryCodesAndOutputTypes[0].first;
							outputType = outputsDeliveryCodesAndOutputTypes[0].second;
						}
					}
					else if (!monitorHLS && deliveryCode != -1)
					{
						// no monitorHLS and delivery code received as input
						bool deliveryCodeFound = false;

						for (pair<int64_t, string> deliveryCodeAndOutputType: outputsDeliveryCodesAndOutputTypes)
						{
							if (deliveryCodeAndOutputType.first == deliveryCode)
							{
								deliveryCodeFound = true;

								outputType = deliveryCodeAndOutputType.second;

								break;
							}
						}

						if (!deliveryCodeFound)
						{
							string errorMessage = string("DeliveryCode received does not exist for the ingestionJob")
								+ ", ingestionJobKey: " + to_string(ingestionJobKey)
							;
							_logger->error(__FILEREF__ + errorMessage);

							throw runtime_error(errorMessage);
						}
					}
					else if (monitorHLS && deliveryCode == -1)
					{
						// monitorHLS and no delivery code received as input

						field = "DeliveryCode";
						if (!JSONUtils::isMetadataPresent(ingestionJobRoot, field))
						{
							string errorMessage = string("A Live-LiveRecorder Monitor HLS without DeliveryCode cannot be delivered")
								+ ", ingestionJobKey: " + to_string(ingestionJobKey)
							;
							_logger->error(__FILEREF__ + errorMessage);

							throw runtime_error(errorMessage);
						}
						deliveryCode = JSONUtils::asInt64(ingestionJobRoot, field, 0);
					}
					else if (monitorHLS && deliveryCode != -1)	// requested delivery code (it is an input)
					{
						// monitorHLS and delivery code received as input
						// monitorHLS is selected

						field = "DeliveryCode";
						if (!JSONUtils::isMetadataPresent(ingestionJobRoot, field))
						{
							string errorMessage = string("A Live-LiveRecorder Monitor HLS without DeliveryCode cannot be delivered")
								+ ", ingestionJobKey: " + to_string(ingestionJobKey)
							;
							_logger->error(__FILEREF__ + errorMessage);

							throw runtime_error(errorMessage);
						}
						deliveryCode = JSONUtils::asInt64(ingestionJobRoot, field, 0);
					}

					string deliveryURI;
					string liveFileExtension = "m3u8";
					tuple<string, string, string> liveDeliveryDetails
						= _mmsStorage->getLiveDeliveryDetails(
						_mmsEngineDBFacade, to_string(deliveryCode),
						liveFileExtension, requestWorkspace);
					tie(deliveryURI, ignore, deliveryFileName) =
						liveDeliveryDetails;

					if (authorizationThroughPath)
					{
						time_t expirationTime = chrono::system_clock::to_time_t(chrono::system_clock::now());
						expirationTime += ttlInSeconds;

						string uriToBeSigned;
						{
							string m3u8Suffix(".m3u8");
							if (deliveryURI.size() >= m3u8Suffix.size()
								&& 0 == deliveryURI.compare(deliveryURI.size()-m3u8Suffix.size(), m3u8Suffix.size(), m3u8Suffix))
							{
								size_t endPathIndex = deliveryURI.find_last_of("/");
								if (endPathIndex == string::npos)
									uriToBeSigned = deliveryURI;
								else
									uriToBeSigned = deliveryURI.substr(0, endPathIndex);
							}
							else
								uriToBeSigned = deliveryURI;
						}
						string md5Base64 = getSignedPath(uriToBeSigned, expirationTime);

						deliveryURL = 
							_deliveryProtocol
							+ "://" 
							+ _deliveryHost_authorizationThroughPath
							+ "/token_" + md5Base64 + "," + to_string(expirationTime)
							+ deliveryURI
						;
					}
					else
					{
						int64_t authorizationKey = _mmsEngineDBFacade->createDeliveryAuthorization(
							userKey,
							clientIPAddress,
							-1,	// physicalPathKey,
							deliveryCode,
							deliveryURI,
							ttlInSeconds,
							maxRetries);

						deliveryURL = 
							_deliveryProtocol
							+ "://" 
							+ _deliveryHost_authorizationThroughParameter
							+ deliveryURI
							+ "?token=" + to_string(authorizationKey)
						;
					}

					_logger->info(__FILEREF__ + "createDeliveryAuthorization for LiveRecorder"
						+ ", ingestionJobKey: " + to_string(ingestionJobKey)
						+ ", deliveryCode: " + to_string(deliveryCode)
						+ ", deliveryURL: " + deliveryURL
					);

					/*
					 * old method using the liveURLConfKey ....
					string field = "ConfigurationLabel";
					string configurationLabel = ingestionJobRoot.get(field, "").asString();
					field = "MonitorHLS";
					if (!JSONUtils::isMetadataPresent(ingestionJobRoot, field))
					{
						string errorMessage = string("A Live-LiveRecorder without MonitorHLS cannot be delivered")
							+ ", ingestionJobKey: " + to_string(ingestionJobKey)
						;
						_logger->error(__FILEREF__ + errorMessage);

						throw runtime_error(errorMessage);
					}

					int64_t liveURLConfKey;
					bool warningIfMissing = false;
					pair<int64_t, string> liveURLConfDetails = _mmsEngineDBFacade->getIPChannelConfDetails(
						requestWorkspace->_workspaceKey, configurationLabel, warningIfMissing);
					tie(liveURLConfKey, ignore) = liveURLConfDetails;

					string deliveryURI;
					string liveFileExtension = "m3u8";
					tuple<string, string, string> liveDeliveryDetails
						= _mmsStorage->getLiveDeliveryDetails(
						_mmsEngineDBFacade, to_string(liveURLConfKey),
						liveFileExtension, requestWorkspace);
					tie(deliveryURI, ignore, deliveryFileName) =
						liveDeliveryDetails;

					authorizationKey = _mmsEngineDBFacade->createDeliveryAuthorization(
						userKey,
						clientIPAddress,
						physicalPathKey,
						liveURLConfKey,
						deliveryURI,
						ttlInSeconds,
						maxRetries);

					deliveryURL = 
						_deliveryProtocol
						+ "://" 
						+ _deliveryHost
						+ deliveryURI
						+ "?token=" + to_string(authorizationKey)
					;
					*/
				}
				else if (ingestionType == MMSEngineDBFacade::IngestionType::AwaitingTheBeginning)
				{
					string field = "DeliveryCode";
					if (!JSONUtils::isMetadataPresent(ingestionJobRoot, field))
					{
						string errorMessage = string("A Awaiting-The-Beginning without DeliveryCode cannot be delivered")
							+ ", ingestionJobKey: " + to_string(ingestionJobKey)
						;
						_logger->error(__FILEREF__ + errorMessage);

						throw runtime_error(errorMessage);
					}
					deliveryCode = JSONUtils::asInt64(ingestionJobRoot, field, 0);

					string deliveryURI;
					string liveFileExtension = "m3u8";
					tuple<string, string, string> liveDeliveryDetails
						= _mmsStorage->getLiveDeliveryDetails(
						_mmsEngineDBFacade, to_string(deliveryCode),
						liveFileExtension, requestWorkspace);
					tie(deliveryURI, ignore, deliveryFileName) =
						liveDeliveryDetails;

					if (authorizationThroughPath)
					{
						time_t expirationTime = chrono::system_clock::to_time_t(chrono::system_clock::now());
						expirationTime += ttlInSeconds;

						string uriToBeSigned;
						{
							string m3u8Suffix(".m3u8");
							if (deliveryURI.size() >= m3u8Suffix.size()
								&& 0 == deliveryURI.compare(deliveryURI.size()-m3u8Suffix.size(), m3u8Suffix.size(), m3u8Suffix))
							{
								size_t endPathIndex = deliveryURI.find_last_of("/");
								if (endPathIndex == string::npos)
									uriToBeSigned = deliveryURI;
								else
									uriToBeSigned = deliveryURI.substr(0, endPathIndex);
							}
							else
								uriToBeSigned = deliveryURI;
						}
						string md5Base64 = getSignedPath(uriToBeSigned, expirationTime);

						deliveryURL = 
							_deliveryProtocol
							+ "://" 
							+ _deliveryHost_authorizationThroughPath
							+ "/token_" + md5Base64 + "," + to_string(expirationTime)
							+ deliveryURI
						;
					}
					else
					{
						int64_t authorizationKey = _mmsEngineDBFacade->createDeliveryAuthorization(
							userKey,
							clientIPAddress,
							-1,	// physicalPathKey,
							deliveryCode,
							deliveryURI,
							ttlInSeconds,
							maxRetries);

						deliveryURL = 
							_deliveryProtocol
							+ "://" 
							+ _deliveryHost_authorizationThroughParameter
							+ deliveryURI
							+ "?token=" + to_string(authorizationKey)
						;
					}

					_logger->info(__FILEREF__ + "createDeliveryAuthorization for AwaitingTheBeginning"
						+ ", ingestionJobKey: " + to_string(ingestionJobKey)
						+ ", deliveryCode: " + to_string(deliveryCode)
						+ ", deliveryURL: " + deliveryURL
					);
				}
				else // if (ingestionType != MMSEngineDBFacade::IngestionType::LiveGrid)
				{
					string field = "DeliveryCode";
					if (!JSONUtils::isMetadataPresent(ingestionJobRoot, field))
					{
						string errorMessage = string("A LiveGrid without DeliveryCode cannot be delivered")
							+ ", ingestionJobKey: " + to_string(ingestionJobKey)
						;
						_logger->error(__FILEREF__ + errorMessage);

						throw runtime_error(errorMessage);
					}
					deliveryCode = JSONUtils::asInt64(ingestionJobRoot, field, 0);

					string deliveryURI;
					string liveFileExtension = "m3u8";
					tuple<string, string, string> liveDeliveryDetails
						= _mmsStorage->getLiveDeliveryDetails(
						_mmsEngineDBFacade, to_string(deliveryCode),
						liveFileExtension, requestWorkspace);
					tie(deliveryURI, ignore, deliveryFileName) =
						liveDeliveryDetails;

					if (authorizationThroughPath)
					{
						time_t expirationTime = chrono::system_clock::to_time_t(chrono::system_clock::now());
						expirationTime += ttlInSeconds;

						string uriToBeSigned;
						{
							string m3u8Suffix(".m3u8");
							if (deliveryURI.size() >= m3u8Suffix.size()
								&& 0 == deliveryURI.compare(deliveryURI.size()-m3u8Suffix.size(), m3u8Suffix.size(), m3u8Suffix))
							{
								size_t endPathIndex = deliveryURI.find_last_of("/");
								if (endPathIndex == string::npos)
									uriToBeSigned = deliveryURI;
								else
									uriToBeSigned = deliveryURI.substr(0, endPathIndex);
							}
							else
								uriToBeSigned = deliveryURI;
						}
						string md5Base64 = getSignedPath(uriToBeSigned, expirationTime);

						deliveryURL = 
							_deliveryProtocol
							+ "://" 
							+ _deliveryHost_authorizationThroughPath
							+ "/token_" + md5Base64 + "," + to_string(expirationTime)
							+ deliveryURI
						;
					}
					else
					{
						int64_t authorizationKey = _mmsEngineDBFacade->createDeliveryAuthorization(
							userKey,
							clientIPAddress,
							-1,	// physicalPathKey,
							deliveryCode,
							deliveryURI,
							ttlInSeconds,
							maxRetries);

						deliveryURL = 
							_deliveryProtocol
							+ "://" 
							+ _deliveryHost_authorizationThroughParameter
							+ deliveryURI
							+ "?token=" + to_string(authorizationKey)
						;
					}

					_logger->info(__FILEREF__ + "createDeliveryAuthorization for LiveGrid"
						+ ", ingestionJobKey: " + to_string(ingestionJobKey)
						+ ", deliveryCode: " + to_string(deliveryCode)
						+ ", deliveryURL: " + deliveryURL
					);
				}
			}

            if (redirect)
            {
				sendRedirect(request, deliveryURL);
            }
            else
            {
                string responseBody = string("{ ")
                    + "\"deliveryURL\": \"" + deliveryURL + "\""
                    + ", \"deliveryFileName\": \"" + deliveryFileName + "\""
                    + ", \"ttlInSeconds\": " + to_string(ttlInSeconds)
                    + ", \"maxRetries\": " + to_string(maxRetries)
                    + " }";
                sendSuccess(request, 201, responseBody);
            }
        }
        catch(runtime_error e)
        {
            _logger->error(__FILEREF__ + api + " failed"
                + ", e.what(): " + e.what()
            );

            string errorMessage = string("Internal server error: ") + e.what();
            _logger->error(__FILEREF__ + errorMessage);

            sendError(request, 500, errorMessage);

            throw runtime_error(errorMessage);
        }
        catch(exception e)
        {
            _logger->error(__FILEREF__ + api + " failed"
                + ", e.what(): " + e.what()
            );

            string errorMessage = string("Internal server error");
            _logger->error(__FILEREF__ + errorMessage);

            sendError(request, 500, errorMessage);

            throw runtime_error(errorMessage);
        }
    }
    catch(runtime_error e)
    {
        _logger->error(__FILEREF__ + "API failed"
            + ", API: " + api
            + ", e.what(): " + e.what()
        );

        throw e;
    }
    catch(exception e)
    {
        _logger->error(__FILEREF__ + "API failed"
            + ", API: " + api
            + ", e.what(): " + e.what()
        );

        string errorMessage = string("Internal server error");
        _logger->error(__FILEREF__ + errorMessage);

        sendError(request, 500, errorMessage);

        throw runtime_error(errorMessage);
    }
}

int64_t API::checkDeliveryAuthorizationThroughParameter(
		string contentURI, string tokenParameter)
{
	int64_t tokenComingFromURL;
	try
	{
		_logger->info(__FILEREF__ + "checkDeliveryAuthorizationThroughParameter, received"
			+ ", contentURI: " + contentURI
			+ ", tokenParameter: " + tokenParameter
		);

		string firstPartOfToken;
		string secondPartOfToken;
		{
			// token formats:
			// scenario in case of .ts (hls) delivery: <encryption of 'manifestLine+++token'>---<cookie: encription of 'token'>
			// scenario in case of .m4s (dash) delivery: ---<cookie: encription of 'token'>
			//		both encryption were built in 'manageHTTPStreamingManifest'
			// scenario in case of .m3u8 delivery:
			//		case 1. master .m3u8: <token>---
			//		case 2. secondary .m3u8: <encryption of 'manifestLine+++token'>---<cookie: encription of 'token'>
			// scenario in case of any other delivery: <token>---
			//		Any other delivery includes for example also the delivery/download of a .ts file, not part
			//		of an hls delivery. In this case we will have just a token as any normal download

			string separator = "---";
			size_t endOfTokenIndex = tokenParameter.rfind(separator);
			if (endOfTokenIndex == string::npos)
			{
				string errorMessage = string("Wrong token format, no --- is present")
					+ ", contentURI: " + contentURI
					+ ", tokenParameter: " + tokenParameter
				;
				_logger->warn(__FILEREF__ + errorMessage);

				throw runtime_error(errorMessage);
			}
			firstPartOfToken = tokenParameter.substr(0, endOfTokenIndex);
			secondPartOfToken = tokenParameter.substr(endOfTokenIndex + separator.length());
		}

		// end with
		string tsExtension(".ts");		// hls
		string m4sExtension(".m4s");	// dash
		string m3u8Extension(".m3u8");	// m3u8
		if (
			(secondPartOfToken != "")	// secondPartOfToken is the cookie
			&&
			(
			(contentURI.size() >= tsExtension.size() && 0 == contentURI.compare(
			contentURI.size()-tsExtension.size(), tsExtension.size(), tsExtension))
			||
			(contentURI.size() >= m4sExtension.size() && 0 == contentURI.compare(
			contentURI.size()-m4sExtension.size(), m4sExtension.size(), m4sExtension))
			||
			(contentURI.size() >= m3u8Extension.size() && 0 == contentURI.compare(
			contentURI.size()-m3u8Extension.size(), m3u8Extension.size(), m3u8Extension)
			&& secondPartOfToken != "")
			)
		)
		{
			// .ts/m4s content to be authorized

			string encryptedToken = firstPartOfToken;
			string cookie = secondPartOfToken;

			if (cookie == "")
			{
				string errorMessage = string("cookie is wrong")
					+ ", contentURI: " + contentURI
					+ ", cookie: " + cookie
					+ ", firstPartOfToken: " + firstPartOfToken
					+ ", secondPartOfToken: " + secondPartOfToken
				;
				_logger->info(__FILEREF__ + errorMessage);

				throw runtime_error(errorMessage);
			}
			// manifestLineAndToken comes from ts URL
			string manifestLineAndToken = Encrypt::decrypt(encryptedToken);
			string manifestLine;
			// int64_t tokenComingFromURL;
			{
				string separator = "+++";
				size_t beginOfTokenIndex = manifestLineAndToken.rfind(separator);
				if (beginOfTokenIndex == string::npos)
				{
					string errorMessage = string("Wrong parameter format")
						+ ", contentURI: " + contentURI
						+ ", manifestLineAndToken: " + manifestLineAndToken
						;
					_logger->info(__FILEREF__ + errorMessage);

					throw runtime_error(errorMessage);
				}
				manifestLine = manifestLineAndToken.substr(0, beginOfTokenIndex);
				string sTokenComingFromURL = manifestLineAndToken.substr(beginOfTokenIndex + separator.length());
				tokenComingFromURL = stoll(sTokenComingFromURL);
			}

			string sTokenComingFromCookie = Encrypt::decrypt(cookie);
			int64_t tokenComingFromCookie = stoll(sTokenComingFromCookie);

			_logger->info(__FILEREF__ + "check token info"
				+ ", encryptedToken: " + encryptedToken
				+ ", manifestLineAndToken: " + manifestLineAndToken
				+ ", manifestLine: " + manifestLine
				+ ", tokenComingFromURL: " + to_string(tokenComingFromURL)
				+ ", cookie: " + cookie
				+ ", sTokenComingFromCookie: " + sTokenComingFromCookie
				+ ", tokenComingFromCookie: " + to_string(tokenComingFromCookie)
				+ ", contentURI: " + contentURI
			);

			if (tokenComingFromCookie != tokenComingFromURL

					// i.e., contentURI: /MMSLive/1/94/94446.ts, manifestLine: 94446.ts
					// 2020-02-04: commented because it does not work in case of dash
					// contentURI: /MMSLive/1/109/init-stream0.m4s
					// manifestLine: chunk-stream$RepresentationID$-$Number%05d$.m4s
					// || contentURI.find(manifestLine) == string::npos
			)
			{
				string errorMessage = string("Wrong parameter format")
					+ ", contentURI: " + contentURI
					+ ", manifestLine: " + manifestLine
					+ ", tokenComingFromCookie: " + to_string(tokenComingFromCookie)
					+ ", tokenComingFromURL: " + to_string(tokenComingFromURL)
					;
				_logger->info(__FILEREF__ + errorMessage);

				throw runtime_error(errorMessage);
			}

			_logger->info(__FILEREF__ + "token authorized"
				+ ", contentURI: " + contentURI
				+ ", manifestLine: " + manifestLine
				+ ", tokenComingFromURL: " + to_string(tokenComingFromURL)
				+ ", tokenComingFromCookie: " + to_string(tokenComingFromCookie)
			);

			// authorized
			// string responseBody;
			// sendSuccess(request, 200, responseBody);
		}
		/*
		else if (
			(contentURI.size() >= m4sExtension.size() && 0 == contentURI.compare(
			contentURI.size()-m4sExtension.size(), m4sExtension.size(), m4sExtension))
		)
		{
			// .m4s content to be authorized

			string encryptedToken = firstPartOfToken;
			string cookie = secondPartOfToken;

			// manifestLineAndToken comes from ts URL
			//string manifestLineAndToken = Encrypt::decrypt(encryptedToken);
			//string manifestLine;
			//int64_t tokenComingFromURL;
			//{
			//	string separator = "+++";
			//	size_t beginOfTokenIndex = manifestLineAndToken.rfind(separator);
			//	if (beginOfTokenIndex == string::npos)
			//	{
			//		string errorMessage = string("Wrong parameter format")
			//			+ ", contentURI: " + contentURI
			//			+ ", manifestLineAndToken: " + manifestLineAndToken
			//			;
			//		_logger->info(__FILEREF__ + errorMessage);
//
//					throw runtime_error(errorMessage);
//				}
//				manifestLine = manifestLineAndToken.substr(0, beginOfTokenIndex);
//				string sTokenComingFromURL = manifestLineAndToken.substr(beginOfTokenIndex + separator.length());
//				tokenComingFromURL = stoll(sTokenComingFromURL);
//			}

			string sTokenComingFromCookie = Encrypt::decrypt(cookie);
			int64_t tokenComingFromCookie = stoll(sTokenComingFromCookie);

//			if (tokenComingFromCookie != tokenComingFromURL
//
//					// i.e., contentURI: /MMSLive/1/94/94446.ts, manifestLine: 94446.ts
//					|| contentURI.find(manifestLine) == string::npos)
//			{
//				string errorMessage = string("Wrong parameter format")
//					+ ", contentURI: " + contentURI
//					+ ", manifestLine: " + manifestLine
//					+ ", tokenComingFromCookie: " + to_string(tokenComingFromCookie)
//					+ ", tokenComingFromURL: " + to_string(tokenComingFromURL)
//					;
//				_logger->info(__FILEREF__ + errorMessage);
//
//				throw runtime_error(errorMessage);
//			}

			_logger->info(__FILEREF__ + "token authorized"
				+ ", contentURI: " + contentURI
				// + ", manifestLine: " + manifestLine
				// + ", tokenComingFromURL: " + to_string(tokenComingFromURL)
				+ ", tokenComingFromCookie: " + to_string(tokenComingFromCookie)
			);

			string responseBody;
			sendSuccess(request, 200, responseBody);
		}
		*/
		else
		{
			tokenComingFromURL = stoll(firstPartOfToken);
			if (_mmsEngineDBFacade->checkDeliveryAuthorization(tokenComingFromURL, contentURI))
			{
				_logger->info(__FILEREF__ + "token authorized"
					+ ", tokenComingFromURL: " + to_string(tokenComingFromURL)
				);

				// authorized

				// string responseBody;
				// sendSuccess(request, 200, responseBody);
			}
			else
			{
				string errorMessage = string("Not authorized: token invalid")
					+ ", tokenComingFromURL: " + to_string(tokenComingFromURL)
                          ;
				_logger->warn(__FILEREF__ + errorMessage);

				throw runtime_error(errorMessage);
			}
		}
	}
	catch(runtime_error e)
	{
		string errorMessage = string("Not authorized");
		_logger->warn(__FILEREF__ + errorMessage);

		throw e;
	}
	catch(exception e)
	{
		string errorMessage = string("Not authorized: exception managing token");
		_logger->warn(__FILEREF__ + errorMessage);

		throw e;
	}

	return tokenComingFromURL;
}

int64_t API::checkDeliveryAuthorizationThroughPath(
	string contentURI)
{
	int64_t tokenComingFromURL = -1;
	try
	{
		_logger->info(__FILEREF__ + "checkDeliveryAuthorizationThroughPath, received"
			+ ", contentURI: " + contentURI
		);

		string tokenLabel = "/token_";

		size_t startTokenIndex = contentURI.find("/token_");
		if (startTokenIndex == string::npos)
		{
			string errorMessage = string("Wrong token format")
				+ ", contentURI: " + contentURI
			;
			_logger->warn(__FILEREF__ + errorMessage);

			throw runtime_error(errorMessage);
		}

		startTokenIndex += tokenLabel.size();

		size_t endTokenIndex = contentURI.find(",", startTokenIndex);
		if (endTokenIndex == string::npos)
		{
			string errorMessage = string("Wrong token format")
				+ ", contentURI: " + contentURI
			;
			_logger->warn(__FILEREF__ + errorMessage);

			throw runtime_error(errorMessage);
		}

		size_t endExpirationIndex = contentURI.find("/", endTokenIndex);
		if (endExpirationIndex == string::npos)
		{
			string errorMessage = string("Wrong token format")
				+ ", contentURI: " + contentURI
			;
			_logger->warn(__FILEREF__ + errorMessage);

			throw runtime_error(errorMessage);
		}

		string tokenSigned = contentURI.substr(startTokenIndex, endTokenIndex - startTokenIndex);
		string sExpirationTime = contentURI.substr(endTokenIndex + 1, endExpirationIndex - (endTokenIndex + 1));
		time_t expirationTime = stoll(sExpirationTime);



		string contentURIToBeVerified;

		size_t endContentURIIndex = contentURI.find("?", endExpirationIndex);
		if (endContentURIIndex == string::npos)
			contentURIToBeVerified = contentURI.substr(endExpirationIndex);
		else
			contentURIToBeVerified = contentURI.substr(endExpirationIndex, endContentURIIndex - endExpirationIndex);

		string m3u8Suffix(".m3u8");
		string tsSuffix(".ts");
		if (contentURIToBeVerified.size() >= m3u8Suffix.size()
			&& 0 == contentURIToBeVerified.compare(contentURIToBeVerified.size()-m3u8Suffix.size(), m3u8Suffix.size(), m3u8Suffix))
		{
			{
				size_t endPathIndex = contentURIToBeVerified.find_last_of("/");
				if (endPathIndex != string::npos)
					contentURIToBeVerified = contentURIToBeVerified.substr(0, endPathIndex);
			}

			string md5Base64 = getSignedPath(contentURIToBeVerified, expirationTime);

			_logger->info(__FILEREF__ + "Authorization through path (m3u8)"
				+ ", contentURI: " + contentURI
				+ ", contentURIToBeVerified: " + contentURIToBeVerified
				+ ", expirationTime: " + to_string(expirationTime)
				+ ", tokenSigned: " + tokenSigned
				+ ", md5Base64: " + md5Base64
			);

			if (md5Base64 != tokenSigned)
			{
				// we still try removing again the last directory to manage the scenario
				// of multi bitrate encoding or multi audio tracks (one director for each audio)
				{
					size_t endPathIndex = contentURIToBeVerified.find_last_of("/");
					if (endPathIndex != string::npos)
						contentURIToBeVerified = contentURIToBeVerified.substr(0, endPathIndex);
				}

				string md5Base64 = getSignedPath(contentURIToBeVerified, expirationTime);

				_logger->info(__FILEREF__ + "Authorization through path (m3u8 2)"
					+ ", contentURI: " + contentURI
					+ ", contentURIToBeVerified: " + contentURIToBeVerified
					+ ", expirationTime: " + to_string(expirationTime)
					+ ", tokenSigned: " + tokenSigned
					+ ", md5Base64: " + md5Base64
				);

				if (md5Base64 != tokenSigned)
				{
					string errorMessage = string("Wrong token (m3u8)")
						+ ", md5Base64: " + md5Base64
						+ ", tokenSigned: " + tokenSigned
					;
					_logger->warn(__FILEREF__ + errorMessage);

					throw runtime_error(errorMessage);
				}
			}
		}
		else if (contentURIToBeVerified.size() >= tsSuffix.size()
			&& 0 == contentURIToBeVerified.compare(contentURIToBeVerified.size()-tsSuffix.size(), tsSuffix.size(), tsSuffix))
		{
			{
				size_t endPathIndex = contentURIToBeVerified.find_last_of("/");
				if (endPathIndex != string::npos)
					contentURIToBeVerified = contentURIToBeVerified.substr(0, endPathIndex);
			}

			string md5Base64 = getSignedPath(contentURIToBeVerified, expirationTime);

			_logger->info(__FILEREF__ + "Authorization through path (ts 1)"
				+ ", contentURI: " + contentURI
				+ ", contentURIToBeVerified: " + contentURIToBeVerified
				+ ", expirationTime: " + to_string(expirationTime)
				+ ", tokenSigned: " + tokenSigned
				+ ", md5Base64: " + md5Base64
			);

			if (md5Base64 != tokenSigned)
			{
				// we still try removing again the last directory to manage
				// the scenario of multi bitrate encoding
				{
					size_t endPathIndex = contentURIToBeVerified.find_last_of("/");
					if (endPathIndex != string::npos)
						contentURIToBeVerified = contentURIToBeVerified.substr(0, endPathIndex);
				}

				string md5Base64 = getSignedPath(contentURIToBeVerified, expirationTime);

				_logger->info(__FILEREF__ + "Authorization through path (ts 2)"
					+ ", contentURI: " + contentURI
					+ ", contentURIToBeVerified: " + contentURIToBeVerified
					+ ", expirationTime: " + to_string(expirationTime)
					+ ", tokenSigned: " + tokenSigned
					+ ", md5Base64: " + md5Base64
				);

				if (md5Base64 != tokenSigned)
				{
					string errorMessage = string("Wrong token (ts)")
						+ ", md5Base64: " + md5Base64
						+ ", tokenSigned: " + tokenSigned
					;
					_logger->warn(__FILEREF__ + errorMessage);

					throw runtime_error(errorMessage);
				}
			}
		}
		else
		{
			string md5Base64 = getSignedPath(contentURIToBeVerified, expirationTime);

			_logger->info(__FILEREF__ + "Authorization through path"
				+ ", contentURI: " + contentURI
				+ ", contentURIToBeVerified: " + contentURIToBeVerified
				+ ", expirationTime: " + to_string(expirationTime)
				+ ", tokenSigned: " + tokenSigned
				+ ", md5Base64: " + md5Base64
			);

			if (md5Base64 != tokenSigned)
			{
				string errorMessage = string("Wrong token")
					+ ", md5Base64: " + md5Base64
					+ ", tokenSigned: " + tokenSigned
				;
				_logger->warn(__FILEREF__ + errorMessage);

				throw runtime_error(errorMessage);
			}
		}


		time_t utcNow = chrono::system_clock::to_time_t(chrono::system_clock::now());
		if (expirationTime < utcNow)
		{
			string errorMessage = string("Token expired")
				+ ", expirationTime: " + to_string(expirationTime)
				+ ", utcNow: " + to_string(utcNow)
			;
			_logger->warn(__FILEREF__ + errorMessage);

			throw runtime_error(errorMessage);
		}
	}
	catch(runtime_error e)
	{
		string errorMessage = string("Not authorized");
		_logger->warn(__FILEREF__ + errorMessage);

		throw e;
	}
	catch(exception e)
	{
		string errorMessage = string("Not authorized: exception managing token");
		_logger->warn(__FILEREF__ + errorMessage);

		throw e;
	}

	return tokenComingFromURL;
}

string API::getSignedPath(string contentURI, time_t expirationTime)
{
	string token = to_string(expirationTime) + contentURI;
	string md5Base64;
	{
		unsigned char digest[MD5_DIGEST_LENGTH];
		MD5((unsigned char*) token.c_str(), token.size(), digest);
		md5Base64 = Convert::base64_encode(digest, MD5_DIGEST_LENGTH);

		transform(md5Base64.begin(), md5Base64.end(), md5Base64.begin(),
			[](unsigned char c){
				if (c == '+')
					return '-';
				else if (c == '/')
					return '_';
				else
					return (char) c;
			}
		);
	}

	_logger->info(__FILEREF__ + "Authorization through path"
		+ ", contentURI: " + contentURI
		+ ", expirationTime: " + to_string(expirationTime)
		+ ", token: " + token
		+ ", md5Base64: " + md5Base64
	);


	return md5Base64;
}

void API::createDeliveryCDN77Authorization(
        FCGX_Request& request,
        int64_t userKey,
        shared_ptr<Workspace> requestWorkspace,
        string clientIPAddress,
        unordered_map<string, string> queryParameters)
{
	string api = "createDeliveryCDN77Authorization";

	_logger->info(__FILEREF__ + "Received " + api
	);

    try
    {
        auto confKeyIt = queryParameters.find("confKey");
        if (confKeyIt != queryParameters.end())
		{
            string errorMessage = string("The 'confKey' parameter has to be present");
            _logger->error(__FILEREF__ + errorMessage);

            sendError(request, 400, errorMessage);

            throw runtime_error(errorMessage);
		}
		int64_t liveURLConfKey = stoll(confKeyIt->second);

		//  It's smart to set the expiration time as current time plus 5 minutes { time() + 300}.
		//  This way the link will be available only for the time needed to start the download.
		//  Afterwards a new link must be generated in order to download this content again.
        auto expireTimestampInSecondsIt = queryParameters.find("expireTimestampInSeconds");
        if (expireTimestampInSecondsIt != queryParameters.end())
		{
            string errorMessage = string("The 'expireTimestampInSeconds' parameter has to be present");
            _logger->error(__FILEREF__ + errorMessage);

            sendError(request, 400, errorMessage);

            throw runtime_error(errorMessage);
		}
		int64_t expireTimestampInSeconds = stoll(expireTimestampInSecondsIt->second);

		string liveURLData;
		tuple<string, string, string> liveURLConfDetails
			= _mmsEngineDBFacade->getIPChannelConfDetails(
			requestWorkspace->_workspaceKey, liveURLConfKey);
		tie(ignore, ignore, liveURLData) = liveURLConfDetails;

		string cdn77DeliveryURL;
		string cdn77SecureToken;
		{
			Json::Value liveURLDataRoot;
			string errors;
			try
			{
				Json::CharReaderBuilder builder;
				Json::CharReader* reader = builder.newCharReader();

				bool parsingSuccessful = reader->parse(liveURLData.c_str(),
					liveURLData.c_str() + liveURLData.size(), 
					&liveURLDataRoot, &errors);
				delete reader;

				if (!parsingSuccessful)
				{
					string errorMessage = string("liveURLData parsing failed")
						+ ", errors: " + errors
						+ ", json liveURLData: " + liveURLData
                       ;
					_logger->error(__FILEREF__ + errorMessage);

					throw runtime_error(errorMessage);
				}
			}
			catch(exception e)
			{
				string errorMessage = string("liveURLData parsing failed")
					+ ", errors: " + errors
					+ ", json liveURLData: " + liveURLData
				;
				_logger->error(__FILEREF__ + errorMessage);

				throw runtime_error(errorMessage);
			}

			string field = "deliveryURL";
			cdn77DeliveryURL = liveURLDataRoot.get(field, "").asString();

			field = "cdn77SecureToken";
			cdn77SecureToken = liveURLDataRoot.get(field, "").asString();
		}

		string protocolPart;
		string cdnResourceUrl;
		string filePath;
		{
			size_t cdnResourceUrlStart = cdn77DeliveryURL.find("://");
			if (cdnResourceUrlStart == string::npos)
			{
				string errorMessage = string("cdn77DeliveryURL format is wrong")
					+ ", cdn77DeliveryURL: " + cdn77DeliveryURL
				;
				_logger->error(__FILEREF__ + errorMessage);

				throw runtime_error(errorMessage);
			}
			cdnResourceUrlStart += 3;	// "://"
			// protocolPart: https://
			protocolPart = cdn77DeliveryURL.substr(0, cdnResourceUrlStart);

			size_t cdnResourceUrlEnd = cdn77DeliveryURL.find_first_of("/", cdnResourceUrlStart);
			if (cdnResourceUrlEnd == string::npos)
			{
				string errorMessage = string("cdn77DeliveryURL format is wrong")
					+ ", cdn77DeliveryURL: " + cdn77DeliveryURL
				;
				_logger->error(__FILEREF__ + errorMessage);

				throw runtime_error(errorMessage);
			}

			// cdnResourceUrl: 1234456789.rsc.cdn77.org
			cdnResourceUrl = cdn77DeliveryURL.substr(cdnResourceUrlStart, cdnResourceUrlEnd - cdnResourceUrlStart);
			// filePath: /file/playlist/d.m3u8
			filePath = cdn77DeliveryURL.substr(cdnResourceUrlEnd);
		}

		string newDeliveryURL;
		{
			// because of hls/dash, anything included after the last slash (e.g. playlist/{chunk}) shouldn't be part of the path string,
			// for which we generate the secure token. Because of that, everything included after the last slash is stripped.
			// $strippedPath = substr($filePath, 0, strrpos($filePath, '/'));
			size_t fileNameStart = filePath.find_last_of("/");
			if (fileNameStart == string::npos)
			{
				string errorMessage = string("filePath format is wrong")
					+ ", filePath: " + filePath
				;
				_logger->error(__FILEREF__ + errorMessage);

				throw runtime_error(errorMessage);
			}
			string strippedPath = filePath.substr(0, fileNameStart);

			// replace invalid URL query string characters +, =, / with valid characters -, _, ~
			// $invalidChars = ['+','/'];
			// $validChars = ['-','_'];
			// GIU: replace is done below

			// if ($strippedPath[0] != '/') {
			// 	$strippedPath = '/' . $strippedPath;
			// }
			// GIU: our strippedPath already starts with /

			// if ($pos = strpos($strippedPath, '?')) {
			// 	$filePath = substr($strippedPath, 0, $pos);
			// }
			// GIU: our strippedPath does not have ?

			// $hashStr = $strippedPath . $secureToken;
			string hashStr = strippedPath + cdn77SecureToken;

			// if ($expiryTimestamp) {
			// 	$hashStr = $expiryTimestamp . $hashStr;
			// 	$expiryTimestamp = ',' . $expiryTimestamp;
			// }
			hashStr = to_string(expireTimestampInSeconds) + hashStr;
			string sExpiryTimestamp = string(",") + to_string(expireTimestampInSeconds);

			// the URL is however, intensionaly returned with the previously stripped parts (eg. playlist/{chunk}..)
			// return 'http://' . $cdnResourceUrl . '/' .
			// 	str_replace($invalidChars, $validChars, base64_encode(md5($hashStr, TRUE))) .
			// 	$expiryTimestamp . $filePath;
			string md5Base64;
			{
				unsigned char digest[MD5_DIGEST_LENGTH];
				MD5((unsigned char*) hashStr.c_str(), hashStr.size(), digest);
				md5Base64 = Convert::base64_encode(digest, MD5_DIGEST_LENGTH);

				// $invalidChars = ['+','/'];
				// $validChars = ['-','_'];
				transform(md5Base64.begin(), md5Base64.end(), md5Base64.begin(),
					[](unsigned char c){
						if (c == '+')
							return '-';
						else if (c == '/')
							return '_';
						else
							return (char) c;
					}
				);
			}

			newDeliveryURL = protocolPart + cdnResourceUrl + "/" + md5Base64
				+ sExpiryTimestamp + filePath;
		}

		string responseBody;
		{
			Json::Value responseBodyRoot;

			string field = "deliveryURL";
			responseBodyRoot[field] = newDeliveryURL;

			{
				Json::StreamWriterBuilder wbuilder;

				responseBody = Json::writeString(wbuilder, responseBodyRoot);
			}
		}

		sendSuccess(request, 201, responseBody);
	}
	catch(runtime_error e)
	{
		_logger->error(__FILEREF__ + api + " failed"
			+ ", e.what(): " + e.what()
		);

		string errorMessage = string("Internal server error: ") + e.what();
		_logger->error(__FILEREF__ + errorMessage);

		sendError(request, 500, errorMessage);

		throw runtime_error(errorMessage);
	}
	catch(exception e)
	{
		_logger->error(__FILEREF__ + api + " failed"
			+ ", e.what(): " + e.what()
		);

		string errorMessage = string("Internal server error");
		_logger->error(__FILEREF__ + errorMessage);

		sendError(request, 500, errorMessage);

		throw runtime_error(errorMessage);
	}
}

void API::tagsList(
        FCGX_Request& request,
        shared_ptr<Workspace> workspace,
        unordered_map<string, string> queryParameters,
        string requestBody)
{
    string api = "tagsList";

    _logger->info(__FILEREF__ + "Received " + api
        + ", requestBody: " + requestBody
    );

    try
    {
        int start = 0;
        auto startIt = queryParameters.find("start");
        if (startIt != queryParameters.end() && startIt->second != "")
        {
            start = stoll(startIt->second);
        }

        int rows = 10;
        auto rowsIt = queryParameters.find("rows");
        if (rowsIt != queryParameters.end() && rowsIt->second != "")
        {
            rows = stoll(rowsIt->second);
			if (rows > _maxPageSize)
				rows = _maxPageSize;
        }
        
        bool contentTypePresent = false;
        MMSEngineDBFacade::ContentType contentType;
        auto contentTypeIt = queryParameters.find("contentType");
        if (contentTypeIt != queryParameters.end() && contentTypeIt->second != "")
        {
            contentType = MMSEngineDBFacade::toContentType(contentTypeIt->second);
            
            contentTypePresent = true;
        }
        
        {
            Json::Value tagsRoot = _mmsEngineDBFacade->getTagsList(
                    workspace->_workspaceKey, start, rows,
                    contentTypePresent, contentType);

            Json::StreamWriterBuilder wbuilder;
            string responseBody = Json::writeString(wbuilder, tagsRoot);
            
            sendSuccess(request, 200, responseBody);
        }
    }
    catch(runtime_error e)
    {
        _logger->error(__FILEREF__ + "API failed"
            + ", API: " + api
            + ", requestBody: " + requestBody
            + ", e.what(): " + e.what()
        );

        string errorMessage = string("Internal server error: ") + e.what();
        _logger->error(__FILEREF__ + errorMessage);

        sendError(request, 500, errorMessage);

        throw runtime_error(errorMessage);
    }
    catch(exception e)
    {
        _logger->error(__FILEREF__ + "API failed"
            + ", API: " + api
            + ", requestBody: " + requestBody
            + ", e.what(): " + e.what()
        );

        string errorMessage = string("Internal server error");
        _logger->error(__FILEREF__ + errorMessage);

        sendError(request, 500, errorMessage);

        throw runtime_error(errorMessage);
    }
}

void API::parseContentRange(string contentRange,
        long long& contentRangeStart,
        long long& contentRangeEnd,
        long long& contentRangeSize)
{
    // Content-Range: bytes 0-99999/100000

    contentRangeStart   = -1;
    contentRangeEnd     = -1;
    contentRangeSize    = -1;

    try
    {
        string prefix ("bytes ");
        if (!(contentRange.size() >= prefix.size() && 0 == contentRange.compare(0, prefix.size(), prefix)))
        {
            string errorMessage = string("Content-Range does not start with 'bytes '")
                    + ", contentRange: " + contentRange
                    ;
            _logger->error(__FILEREF__ + errorMessage);

            throw runtime_error(errorMessage);
        }

        int startIndex = prefix.size();
        int endIndex = contentRange.find("-", startIndex);
        if (endIndex == string::npos)
        {
            string errorMessage = string("Content-Range does not have '-'")
                    + ", contentRange: " + contentRange
                    ;
            _logger->error(__FILEREF__ + errorMessage);

            throw runtime_error(errorMessage);
        }

        contentRangeStart = stoll(contentRange.substr(startIndex, endIndex - startIndex));

        endIndex++;
        int sizeIndex = contentRange.find("/", endIndex);
        if (sizeIndex == string::npos)
        {
            string errorMessage = string("Content-Range does not have '/'")
                    + ", contentRange: " + contentRange
                    ;
            _logger->error(__FILEREF__ + errorMessage);

            throw runtime_error(errorMessage);
        }

        contentRangeEnd = stoll(contentRange.substr(endIndex, sizeIndex - endIndex));

        sizeIndex++;
        contentRangeSize = stoll(contentRange.substr(sizeIndex));
    }
    catch(exception e)
    {
        string errorMessage = string("Content-Range is not well done. Expected format: 'Content-Range: bytes <start>-<end>/<size>'")
            + ", contentRange: " + contentRange
        ;
        _logger->error(__FILEREF__ + errorMessage);

        throw runtime_error(errorMessage);            
    }
}

void API::mmsSupport(
        FCGX_Request& request,
		int64_t userKey, string apiKey,
        shared_ptr<Workspace> workspace,
        unordered_map<string, string> queryParameters,
        string requestBody)
{
    string api = "mmsSupport";

    _logger->info(__FILEREF__ + "Received " + api
        + ", requestBody: " + requestBody
    );

    try
    {
        string userEmailAddress;
        string subject;
        string text;

        Json::Value metadataRoot;
        try
        {
            Json::CharReaderBuilder builder;
            Json::CharReader* reader = builder.newCharReader();
            string errors;

            bool parsingSuccessful = reader->parse(requestBody.c_str(),
                    requestBody.c_str() + requestBody.size(), 
                    &metadataRoot, &errors);
            delete reader;

            if (!parsingSuccessful)
            {
                string errorMessage = string("Json metadata failed during the parsing")
                        + ", errors: " + errors
                        + ", json data: " + requestBody
                        ;
                _logger->error(__FILEREF__ + errorMessage);

                sendError(request, 400, errorMessage);

                throw runtime_error(errorMessage);
            }
        }
        catch(exception e)
        {
            string errorMessage = string("Json metadata failed during the parsing"
                    ", json data: " + requestBody
                    );
            _logger->error(__FILEREF__ + errorMessage);

            sendError(request, 400, errorMessage);

            throw runtime_error(errorMessage);
        }

		vector<string> mandatoryFields = {
			"UserEmailAddress",
			"Subject",
			"Text"
		};
		for (string field: mandatoryFields)
		{
			if (!JSONUtils::isMetadataPresent(metadataRoot, field))
			{
				string errorMessage = string("Json field is not present or it is null")
					+ ", Json field: " + field;
				_logger->error(__FILEREF__ + errorMessage);

				sendError(request, 400, errorMessage);

				throw runtime_error(errorMessage);
			}
		}

		userEmailAddress = metadataRoot.get("UserEmailAddress", "").asString();
		subject = metadataRoot.get("Subject", "").asString();
		text = metadataRoot.get("Text", "").asString();

        try
        {
            string to = "mms.technical.support@catrasoft.cloud";

            vector<string> emailBody;
            emailBody.push_back(string("<p>UserKey: ") + to_string(userKey) + "</p>");
            emailBody.push_back(string("<p>WorkspaceKey: ")
				+ to_string(workspace->_workspaceKey) + "</p>");
            emailBody.push_back(string("<p>APIKey: ") + apiKey + "</p>");
            emailBody.push_back(string("<p></p>"));
            emailBody.push_back(string("<p>From: ") + userEmailAddress + "</p>");
            emailBody.push_back(string("<p></p>"));
            emailBody.push_back(string("<p>") + text + "</p>");

            EMailSender emailSender(_logger, _configuration);
            emailSender.sendEmail(to, subject, emailBody);

            string responseBody;
            sendSuccess(request, 201, responseBody);
        }
        catch(runtime_error e)
        {
            _logger->error(__FILEREF__ + api + " failed"
                + ", e.what(): " + e.what()
            );

            string errorMessage = string("Internal server error: ") + e.what();
            _logger->error(__FILEREF__ + errorMessage);

            sendError(request, 500, errorMessage);

            throw runtime_error(errorMessage);
        }
        catch(exception e)
        {
            _logger->error(__FILEREF__ + api + " failed"
                + ", e.what(): " + e.what()
            );

            string errorMessage = string("Internal server error");
            _logger->error(__FILEREF__ + errorMessage);

            sendError(request, 500, errorMessage);

            throw runtime_error(errorMessage);
        }
    }
    catch(runtime_error e)
    {
        _logger->error(__FILEREF__ + "API failed"
            + ", API: " + api
            + ", requestBody: " + requestBody
            + ", e.what(): " + e.what()
        );

        throw e;
    }
    catch(exception e)
    {
        _logger->error(__FILEREF__ + "API failed"
            + ", API: " + api
            + ", requestBody: " + requestBody
            + ", e.what(): " + e.what()
        );

        string errorMessage = string("Internal server error");
        _logger->error(__FILEREF__ + errorMessage);

        sendError(request, 500, errorMessage);

        throw runtime_error(errorMessage);
    }
}

