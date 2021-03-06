/*
 * To change this license header, choose License Headers in Project Properties.
 * To change this template file, choose Tools | Templates
 * and open the template in the editor.
 */

/* 
 * File:   EncodersLoadBalancer.cpp
 * Author: giuliano
 * 
 * Created on April 28, 2018, 2:33 PM
 */

#include "EncodersLoadBalancer.h"

EncodersLoadBalancer::EncodersLoadBalancer(
	shared_ptr<MMSEngineDBFacade> mmsEngineDBFacade,
    Json::Value configuration,
    shared_ptr<spdlog::logger> logger
)
{
    _logger             = logger;
	_mmsEngineDBFacade	= mmsEngineDBFacade;

	/*
    Json::Value encodersPools = configuration["ffmpeg"]["hosts"];
    
    for (auto const& encodersPoolName : encodersPools.getMemberNames())
    {
        _logger->info(__FILEREF__ + "encodersPools"
            + ", encodersPoolName: " + encodersPoolName
        );

        EncodersPoolDetails encodersPoolDetails;
        
        encodersPoolDetails._lastEncoderUsed = -1;
        
		// encodersPool will be "common" or "workspaceKey"
        Json::Value encodersPool = encodersPools[encodersPoolName];
        
        for (int encoderPoolIndex = 0; encoderPoolIndex < encodersPool.size(); encoderPoolIndex++)
        {
            string encoderHostName = encodersPool[encoderPoolIndex].asString();
            
            _logger->info(__FILEREF__ + "encodersPool"
                + ", encoderHostName: " + encoderHostName
            );
        
            encodersPoolDetails._encoders.push_back(encoderHostName);
        }
        
        _encodersPools[encodersPoolName] = encodersPoolDetails;
    }
	*/
}

EncodersLoadBalancer::~EncodersLoadBalancer() {
}

/*
string EncodersLoadBalancer::getEncoderHost(string encodersPool, shared_ptr<Workspace> workspace,
	string transcoderToSkip) 
{
    string defaultEncodersPool = "common";

	map<string, EncodersPoolDetails>::iterator it = _encodersPools.end();
	// Priority 1: encodersPool
	if (encodersPool != "")
		it = _encodersPools.find(encodersPool);
	// Priority 2: workspace
	if (it == _encodersPools.end())
		it = _encodersPools.find(workspace->_directoryName);
	// Priority 3: default encoders pool (common)
	if (it == _encodersPools.end())
		it = _encodersPools.find(defaultEncodersPool);

    if (it == _encodersPools.end())
    {
        string errorMessage = "No encoders pools found";
        _logger->error(__FILEREF__ + errorMessage);

        throw runtime_error(errorMessage);                    
    }
    
    it->second._lastEncoderUsed     = (it->second._lastEncoderUsed + 1) % it->second._encoders.size();
	if (transcoderToSkip != "" && transcoderToSkip == it->second._encoders[it->second._lastEncoderUsed])
		it->second._lastEncoderUsed     = (it->second._lastEncoderUsed + 1) % it->second._encoders.size();        

    return it->second._encoders[it->second._lastEncoderUsed];
}
*/

pair<int64_t, string> EncodersLoadBalancer::getEncoderURL(
	string encodersPoolLabel, shared_ptr<Workspace> workspace,
	int64_t encoderKeyToBeSkipped)
{

    _logger->info(__FILEREF__ + "Received getEncoderURL"
			+ ", workspaceKey: " + to_string(workspace->_workspaceKey)
			+ ", encodersPoolLabel: " + encodersPoolLabel
			+ ", encoderKeyToBeSkipped: " + to_string(encoderKeyToBeSkipped)
	);

	try
	{
		tuple<int64_t, string, string, int> encoderByEncodersPool =
			_mmsEngineDBFacade->getEncoderByEncodersPool(
			workspace->_workspaceKey, encodersPoolLabel,
			encoderKeyToBeSkipped);

		int64_t encoderKey;
		string protocol;
		string serverName;
		int port;

		tie(encoderKey, protocol, serverName, port) = encoderByEncodersPool;

		string encoderURL = protocol + "://" + serverName + ":" + to_string(port);

        _logger->info(__FILEREF__ + "getEncoderURL"
			+ ", workspaceKey: " + to_string(workspace->_workspaceKey)
			+ ", encodersPoolLabel: " + encodersPoolLabel
			+ ", encoderKeyToBeSkipped: " + to_string(encoderKeyToBeSkipped)
			+ ", encoderKey: " + to_string(encoderKey)
			+ ", encoderURL: " + encoderURL
        );

		return make_pair(encoderKey, encoderURL);
	}
    catch(EncoderNotFound e)
    {
        _logger->error(__FILEREF__ + "getEncoderURL failed"
			+ ", workspaceKey: " + to_string(workspace->_workspaceKey)
			+ ", encodersPool: " + encodersPoolLabel
			+ ", encoderKeyToBeSkipped: " + to_string(encoderKeyToBeSkipped)
            + ", e.what(): " + e.what()
        );

        throw e;
    }
    catch(runtime_error e)
    {
        _logger->error(__FILEREF__ + "getEncoderURL failed"
			+ ", workspaceKey: " + to_string(workspace->_workspaceKey)
			+ ", encodersPool: " + encodersPoolLabel
			+ ", encoderKeyToBeSkipped: " + to_string(encoderKeyToBeSkipped)
            + ", e.what(): " + e.what()
        );

        throw e;
    }
    catch(exception e)
    {
        string errorMessage = string("getEncoderURL failed")
			+ ", workspaceKey: " + to_string(workspace->_workspaceKey)
			+ ", encodersPool: " + encodersPoolLabel
			+ ", encoderKeyToBeSkipped: " + to_string(encoderKeyToBeSkipped)
            + ", e.what(): " + e.what()
        ;
        _logger->error(__FILEREF__ + errorMessage);

        throw runtime_error(errorMessage);
    }
}

