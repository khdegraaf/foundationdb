/*
 * QuietDatabase.actor.cpp
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2018 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "flow/ActorCollection.h"
#include "fdbrpc/simulator.h"
#include "flow/Trace.h"
#include "fdbclient/NativeAPI.actor.h"
#include "fdbclient/DatabaseContext.h"
#include "fdbserver/TesterInterface.actor.h"
#include "fdbserver/WorkerInterface.actor.h"
#include "fdbserver/ServerDBInfo.h"
#include "fdbserver/Status.h"
#include "fdbclient/ManagementAPI.actor.h"
#include "flow/actorcompiler.h"  // This must be the last #include.

ACTOR Future<vector<std::pair<WorkerInterface, ProcessClass>>> getWorkers( Reference<AsyncVar<ServerDBInfo>> dbInfo, int flags = 0 ) {
	loop {
		choose {
			when( vector<std::pair<WorkerInterface, ProcessClass>> w = wait( brokenPromiseToNever( dbInfo->get().clusterInterface.getWorkers.getReply( GetWorkersRequest( flags ) ) ) ) ) {
				return w;
			}
			when( wait( dbInfo->onChange() ) ) {}
		}
	}
}

//Gets the WorkerInterface representing the Master server.
ACTOR Future<WorkerInterface> getMasterWorker( Database cx, Reference<AsyncVar<ServerDBInfo>> dbInfo ) {
	TraceEvent("GetMasterWorker").detail("Stage", "GettingWorkers");

	loop {
		state vector<std::pair<WorkerInterface, ProcessClass>> workers = wait( getWorkers( dbInfo ) );

		for( int i = 0; i < workers.size(); i++ ) {
			if( workers[i].first.address() == dbInfo->get().master.address() ) {
				TraceEvent("GetMasterWorker").detail("Stage", "GotWorkers").detail("MasterId", dbInfo->get().master.id()).detail("WorkerId", workers[i].first.id());
				return workers[i].first;
			}
		}

		TraceEvent(SevWarn, "GetMasterWorkerError")
			.detail("Error", "MasterWorkerNotFound")
			.detail("Master", dbInfo->get().master.id()).detail("MasterAddress", dbInfo->get().master.address())
			.detail("WorkerCount", workers.size());

		wait(delay(1.0));
	}
}

// Gets the WorkerInterface representing the data distributor.
ACTOR Future<WorkerInterface> getDataDistributorWorker( Database cx, Reference<AsyncVar<ServerDBInfo>> dbInfo ) {
	TraceEvent("GetDataDistributorWorker").detail("Stage", "GettingWorkers");

	loop {
		state vector<std::pair<WorkerInterface, ProcessClass>> workers = wait( getWorkers( dbInfo ) );
		if (!dbInfo->get().distributor.present()) continue;

		for( int i = 0; i < workers.size(); i++ ) {
			if( workers[i].first.address() == dbInfo->get().distributor.get().address() ) {
				TraceEvent("GetDataDistributorWorker").detail("Stage", "GotWorkers")
				.detail("DataDistributorId", dbInfo->get().distributor.get().id())
				.detail("WorkerId", workers[i].first.id());
				return workers[i].first;
			}
		}

		TraceEvent(SevWarn, "GetDataDistributorWorker")
		.detail("Error", "DataDistributorWorkerNotFound")
		.detail("DataDistributorId", dbInfo->get().distributor.get().id())
		.detail("DataDistributorAddress", dbInfo->get().distributor.get().address())
		.detail("WorkerCount", workers.size());
	}
}

// Gets the number of bytes in flight from the data distributor.
ACTOR Future<int64_t> getDataInFlight( Database cx, WorkerInterface distributorWorker ) {
	try {
		TraceEvent("DataInFlight").detail("Stage", "ContactingDataDistributor");
		TraceEventFields md = wait( timeoutError(distributorWorker.eventLogRequest.getReply(
			EventLogRequest( LiteralStringRef("TotalDataInFlight") ) ), 1.0 ) );
		int64_t dataInFlight;
		sscanf(md.getValue("TotalBytes").c_str(), "%lld", &dataInFlight);
		return dataInFlight;
	} catch( Error &e ) {
		TraceEvent("QuietDatabaseFailure", distributorWorker.id()).error(e).detail("Reason", "Failed to extract DataInFlight");
		throw;
	}

}

// Gets the number of bytes in flight from the data distributor.
ACTOR Future<int64_t> getDataInFlight( Database cx, Reference<AsyncVar<ServerDBInfo>> dbInfo ) {
	WorkerInterface distributorInterf = wait( getDataDistributorWorker(cx, dbInfo) );
	int64_t dataInFlight = wait(getDataInFlight(cx, distributorInterf));
	return dataInFlight;
}

//Computes the queue size for storage servers and tlogs using the bytesInput and bytesDurable attributes
int64_t getQueueSize( const TraceEventFields& md ) {
	double inputRate, durableRate;
	double inputRoughness, durableRoughness;
	int64_t inputBytes, durableBytes;

	sscanf(md.getValue("BytesInput").c_str(), "%lf %lf %lld", &inputRate, &inputRoughness, &inputBytes);
	sscanf(md.getValue("BytesDurable").c_str(), "%lf %lf %lld", &durableRate, &durableRoughness, &durableBytes);

	return inputBytes - durableBytes;
}

// This is not robust in the face of a TLog failure
ACTOR Future<int64_t> getMaxTLogQueueSize( Database cx, Reference<AsyncVar<ServerDBInfo>> dbInfo ) {
	TraceEvent("MaxTLogQueueSize").detail("Stage", "ContactingLogs");

	state std::vector<std::pair<WorkerInterface, ProcessClass>> workers = wait(getWorkers(dbInfo));
	std::map<NetworkAddress, WorkerInterface> workersMap;
	for(auto worker : workers) {
		workersMap[worker.first.address()] = worker.first;
	}

	state std::vector<Future<TraceEventFields>> messages;
	state std::vector<TLogInterface> tlogs = dbInfo->get().logSystemConfig.allPresentLogs();
	for(int i = 0; i < tlogs.size(); i++) {
		auto itr = workersMap.find(tlogs[i].address());
		if(itr == workersMap.end()) {
			TraceEvent("QuietDatabaseFailure").detail("Reason", "Could not find worker for log server").detail("Tlog", tlogs[i].id());
			throw attribute_not_found();
		}
		messages.push_back( timeoutError(itr->second.eventLogRequest.getReply(
			EventLogRequest( StringRef(tlogs[i].id().toString() + "/TLogMetrics") ) ), 1.0 ) );
	}
	wait( waitForAll( messages ) );

	TraceEvent("MaxTLogQueueSize").detail("Stage", "ComputingMax").detail("MessageCount", messages.size());

	state int64_t maxQueueSize = 0;
	state int i = 0;
	for(; i < messages.size(); i++) {
		try {
			maxQueueSize = std::max( maxQueueSize, getQueueSize( messages[i].get() ) );
		} catch( Error &e ) {
			TraceEvent("QuietDatabaseFailure").detail("Reason", "Failed to extract MaxTLogQueue").detail("Tlog", tlogs[i].id());
			throw;
		}
	}

	return maxQueueSize;
}

ACTOR Future<vector<StorageServerInterface>> getStorageServers( Database cx, bool use_system_priority = false) {
	state Transaction tr( cx );
	if (use_system_priority)
		tr.setOption(FDBTransactionOptions::PRIORITY_SYSTEM_IMMEDIATE);
		tr.setOption(FDBTransactionOptions::LOCK_AWARE);
	loop {
		try {
			Standalone<RangeResultRef> serverList = wait( tr.getRange( serverListKeys, CLIENT_KNOBS->TOO_MANY ) );
			ASSERT( !serverList.more && serverList.size() < CLIENT_KNOBS->TOO_MANY );

			vector<StorageServerInterface> servers;
			for( int i = 0; i < serverList.size(); i++ )
				servers.push_back( decodeServerListValue( serverList[i].value ) );
			return servers;
		}
		catch(Error &e) {
			wait( tr.onError(e) );
		}
	}
}

//Gets the maximum size of all the storage server queues
ACTOR Future<int64_t> getMaxStorageServerQueueSize( Database cx, Reference<AsyncVar<ServerDBInfo>> dbInfo ) {
	TraceEvent("MaxStorageServerQueueSize").detail("Stage", "ContactingStorageServers");

	Future<std::vector<StorageServerInterface>> serversFuture = getStorageServers(cx);
	state Future<std::vector<std::pair<WorkerInterface, ProcessClass>>> workersFuture = getWorkers(dbInfo);

	state std::vector<StorageServerInterface> servers = wait(serversFuture);
	state std::vector<std::pair<WorkerInterface, ProcessClass>> workers = wait(workersFuture);

	std::map<NetworkAddress, WorkerInterface> workersMap;
	for(auto worker : workers) {
		workersMap[worker.first.address()] = worker.first;
	}

	state std::vector<Future<TraceEventFields>> messages;
	for(int i = 0; i < servers.size(); i++) {
		auto itr = workersMap.find(servers[i].address());
		if(itr == workersMap.end()) {
			TraceEvent("QuietDatabaseFailure").detail("Reason", "Could not find worker for storage server").detail("SS", servers[i].id());
			throw attribute_not_found();
		}
		messages.push_back( timeoutError(itr->second.eventLogRequest.getReply(
			EventLogRequest( StringRef(servers[i].id().toString() + "/StorageMetrics") ) ), 1.0 ) );
	}

	wait( waitForAll(messages) );

	TraceEvent("MaxStorageServerQueueSize").detail("Stage", "ComputingMax").detail("MessageCount", messages.size());

	state int64_t maxQueueSize = 0;
	state int i = 0;
	for(; i < messages.size(); i++) {
		try {
			maxQueueSize = std::max( maxQueueSize, getQueueSize( messages[i].get() ) );
		} catch( Error &e ) {
			TraceEvent("QuietDatabaseFailure").detail("Reason", "Failed to extract MaxStorageServerQueue").detail("SS", servers[i].id());
			throw;
		}
	}

	return maxQueueSize;
}

//Gets the size of the data distribution queue.  If reportInFlight is true, then data in flight is considered part of the queue
ACTOR Future<int64_t> getDataDistributionQueueSize( Database cx, WorkerInterface distributorWorker, bool reportInFlight) {
	try {
		TraceEvent("DataDistributionQueueSize").detail("Stage", "ContactingDataDistributor");

		TraceEventFields movingDataMessage = wait( timeoutError(distributorWorker.eventLogRequest.getReply(
			EventLogRequest( LiteralStringRef("MovingData") ) ), 1.0 ) );

		TraceEvent("DataDistributionQueueSize").detail("Stage", "GotString");

		int64_t inQueue;
		sscanf(movingDataMessage.getValue("InQueue").c_str(), "%lld", &inQueue);

		if(reportInFlight) {
			int64_t inFlight;
			sscanf(movingDataMessage.getValue("InFlight").c_str(), "%lld", &inFlight);
			inQueue += inFlight;
		}

		return inQueue;
	} catch( Error &e ) {
		TraceEvent("QuietDatabaseFailure", distributorWorker.id()).detail("Reason", "Failed to extract DataDistributionQueueSize");
		throw;
	}
}

//Gets the size of the data distribution queue.  If reportInFlight is true, then data in flight is considered part of the queue
//Convenience method that first finds the master worker from a zookeeper interface
ACTOR Future<int64_t> getDataDistributionQueueSize( Database cx, Reference<AsyncVar<ServerDBInfo>> dbInfo, bool reportInFlight ) {
	WorkerInterface distributorInterf = wait( getDataDistributorWorker(cx, dbInfo) );
	int64_t inQueue = wait( getDataDistributionQueueSize( cx, distributorInterf, reportInFlight) );
	return inQueue;
}

// Gets if the number of process and machine teams does not exceed the maximum allowed number of teams
ACTOR Future<bool> getTeamCollectionValid(Database cx, WorkerInterface dataDistributorWorker) {
	state int attempts = 0;
	loop {
		try {
			TraceEvent("GetTeamCollectionValid").detail("Stage", "ContactingMaster");

			TraceEventFields teamCollectionInfoMessage = wait(timeoutError(
			    dataDistributorWorker.eventLogRequest.getReply(EventLogRequest(LiteralStringRef("TeamCollectionInfo"))), 1.0));

			TraceEvent("GetTeamCollectionValid").detail("Stage", "GotString");

			int64_t currentTeamNumber;
			int64_t desiredTeamNumber;
			int64_t maxTeamNumber;
			int64_t currentMachineTeamNumber;
			int64_t healthyMachineTeamCount;
			int64_t desiredMachineTeamNumber;
			int64_t maxMachineTeamNumber;
			sscanf(teamCollectionInfoMessage.getValue("CurrentTeamNumber").c_str(), "%lld", &currentTeamNumber);
			sscanf(teamCollectionInfoMessage.getValue("DesiredTeamNumber").c_str(), "%lld", &desiredTeamNumber);
			sscanf(teamCollectionInfoMessage.getValue("MaxTeamNumber").c_str(), "%lld", &maxTeamNumber);
			sscanf(teamCollectionInfoMessage.getValue("CurrentMachineTeamNumber").c_str(), "%lld",
			       &currentMachineTeamNumber);
			sscanf(teamCollectionInfoMessage.getValue("CurrentHealthyMachineTeamNumber").c_str(), "%lld",
			       &healthyMachineTeamCount);
			sscanf(teamCollectionInfoMessage.getValue("DesiredMachineTeams").c_str(), "%lld",
			       &desiredMachineTeamNumber);
			sscanf(teamCollectionInfoMessage.getValue("MaxMachineTeams").c_str(), "%lld", &maxMachineTeamNumber);

			// Team number is always valid when we disable teamRemover. This avoids false positive in simulation test
			if (SERVER_KNOBS->TR_FLAG_DISABLE_TEAM_REMOVER) {
				TraceEvent("GetTeamCollectionValid")
				    .detail("KnobsTeamRemoverDisabled", SERVER_KNOBS->TR_FLAG_DISABLE_TEAM_REMOVER);
				return true;
			}

			// The if condition should be consistent with the condition in teamRemover() that decides
			// if redundant teams exist.
			if (healthyMachineTeamCount > desiredMachineTeamNumber) {
				TraceEvent("GetTeamCollectionValid")
				    .detail("CurrentTeamNumber", currentTeamNumber)
				    .detail("DesiredTeamNumber", desiredTeamNumber)
				    .detail("MaxTeamNumber", maxTeamNumber)
				    .detail("CurrentHealthyMachineTeamNumber", healthyMachineTeamCount)
				    .detail("DesiredMachineTeams", desiredMachineTeamNumber)
				    .detail("CurrentMachineTeamNumber", currentMachineTeamNumber)
				    .detail("MaxMachineTeams", maxMachineTeamNumber);
				return false;
			} else {
				return true;
			}

		} catch (Error& e) {
			TraceEvent("QuietDatabaseFailure", dataDistributorWorker.id())
			    .detail("Reason", "Failed to extract GetTeamCollectionValid information");
			attempts++;
			if (attempts > 10) {
				TraceEvent("QuietDatabaseNoTeamCollectionInfo", dataDistributorWorker.id())
				    .detail("Reason", "Had never called build team to build any team");
				return true;
			}
			// throw;
			wait(delay(10.0));
		}
	};
}

// Gets if the number of process and machine teams does not exceed the maximum allowed number of teams
// Convenience method that first finds the master worker from a zookeeper interface
ACTOR Future<bool> getTeamCollectionValid(Database cx, Reference<AsyncVar<ServerDBInfo>> dbInfo) {
	WorkerInterface dataDistributorWorker = wait(getDataDistributorWorker(cx, dbInfo));
	bool valid = wait(getTeamCollectionValid(cx, dataDistributorWorker));
	return valid;
}

// Checks that data distribution is active
ACTOR Future<bool> getDataDistributionActive( Database cx, WorkerInterface distributorWorker ) {
	try {
		TraceEvent("DataDistributionActive").detail("Stage", "ContactingDataDistributor");

		TraceEventFields activeMessage = wait( timeoutError(distributorWorker.eventLogRequest.getReply(
			EventLogRequest( LiteralStringRef("DDTrackerStarting") ) ), 1.0 ) );

		return activeMessage.getValue("State") == "Active";
	} catch( Error &e ) {
		TraceEvent("QuietDatabaseFailure", distributorWorker.id()).detail("Reason", "Failed to extract DataDistributionActive");
		throw;
	}
}

// Checks to see if any storage servers are being recruited
ACTOR Future<bool> getStorageServersRecruiting( Database cx, WorkerInterface distributorWorker, UID distributorUID ) {
	try {
		TraceEvent("StorageServersRecruiting").detail("Stage", "ContactingDataDistributor");
		TraceEventFields recruitingMessage = wait( timeoutError(distributorWorker.eventLogRequest.getReply(
			EventLogRequest( StringRef( "StorageServerRecruitment_" + distributorUID.toString()) ) ), 1.0 ) );

		TraceEvent("StorageServersRecruiting").detail("Message", recruitingMessage.toString());
		return recruitingMessage.getValue("State") == "Recruiting";
	} catch( Error &e ) {
		TraceEvent("QuietDatabaseFailure", distributorWorker.id())
			.detail("Reason", "Failed to extract StorageServersRecruiting")
			.detail("DataDistributorID", distributorUID);
		throw;
	}
}

ACTOR Future<Void> repairDeadDatacenter(Database cx, Reference<AsyncVar<ServerDBInfo>> dbInfo, std::string context) {
	if(g_network->isSimulated() && g_simulator.usableRegions > 1) {
		bool primaryDead = g_simulator.datacenterDead(g_simulator.primaryDcId);
		bool remoteDead = g_simulator.datacenterDead(g_simulator.remoteDcId);

		ASSERT(!primaryDead || !remoteDead);
		if(primaryDead || remoteDead) {
			TraceEvent(SevWarnAlways, "DisablingFearlessConfiguration").detail("Location", context).detail("Stage", "Repopulate").detail("RemoteDead", remoteDead).detail("PrimaryDead", primaryDead);
			g_simulator.usableRegions = 1;
			wait(success( changeConfig( cx, (primaryDead ? g_simulator.disablePrimary : g_simulator.disableRemote) + " repopulate_anti_quorum=1", true ) ));
			while( dbInfo->get().recoveryState < RecoveryState::STORAGE_RECOVERED ) {
				wait( dbInfo->onChange() );
			}
			TraceEvent(SevWarnAlways, "DisablingFearlessConfiguration").detail("Location", context).detail("Stage", "Usable_Regions");
			wait(success( changeConfig( cx, "usable_regions=1", true ) ));
		}
	}
	return Void();
}

ACTOR Future<Void> reconfigureAfter(Database cx, double time, Reference<AsyncVar<ServerDBInfo>> dbInfo, std::string context) {
	wait( delay(time) );
	wait( repairDeadDatacenter(cx, dbInfo, context) );
	return Void();
}

ACTOR Future<Void> waitForQuietDatabase( Database cx, Reference<AsyncVar<ServerDBInfo>> dbInfo, std::string phase, int64_t dataInFlightGate = 2e6,
	int64_t maxTLogQueueGate = 5e6, int64_t maxStorageServerQueueGate = 5e6, int64_t maxDataDistributionQueueSize = 0 ) {
	state Future<Void> reconfig = reconfigureAfter(cx, 100 + (g_random->random01()*100), dbInfo, "QuietDatabase");

	TraceEvent(("QuietDatabase" + phase + "Begin").c_str());

	//In a simulated environment, wait 5 seconds so that workers can move to their optimal locations
	if(g_network->isSimulated())
		wait(delay(5.0));

	//Require 3 consecutive successful quiet database checks spaced 2 second apart
	state int numSuccesses = 0;

	loop {
		try {
			TraceEvent("QuietDatabaseWaitingOnDataDistributor");
			WorkerInterface distributorWorker = wait( getDataDistributorWorker( cx, dbInfo ) );
			UID distributorUID = dbInfo->get().distributor.get().id();
			TraceEvent("QuietDatabaseGotDataDistributor", distributorUID).detail("Locality", distributorWorker.locality.toString());

			state Future<int64_t> dataInFlight = getDataInFlight( cx, distributorWorker);
			state Future<int64_t> tLogQueueSize = getMaxTLogQueueSize( cx, dbInfo );
			state Future<int64_t> dataDistributionQueueSize = getDataDistributionQueueSize( cx, distributorWorker, dataInFlightGate == 0);
			state Future<bool> teamCollectionValid = getTeamCollectionValid(cx, distributorWorker);
			state Future<int64_t> storageQueueSize = getMaxStorageServerQueueSize( cx, dbInfo );
			state Future<bool> dataDistributionActive = getDataDistributionActive( cx, distributorWorker );
			state Future<bool> storageServersRecruiting = getStorageServersRecruiting ( cx, distributorWorker, distributorUID );

			wait(success(dataInFlight) && success(tLogQueueSize) && success(dataDistributionQueueSize) &&
			     success(teamCollectionValid) && success(storageQueueSize) && success(dataDistributionActive) &&
			     success(storageServersRecruiting));
			TraceEvent(("QuietDatabase" + phase).c_str())
			    .detail("DataInFlight", dataInFlight.get())
			    .detail("MaxTLogQueueSize", tLogQueueSize.get())
			    .detail("DataDistributionQueueSize", dataDistributionQueueSize.get())
			    .detail("TeamCollectionValid", teamCollectionValid.get())
			    .detail("MaxStorageQueueSize", storageQueueSize.get())
			    .detail("DataDistributionActive", dataDistributionActive.get())
			    .detail("StorageServersRecruiting", storageServersRecruiting.get());

			if (dataInFlight.get() > dataInFlightGate || tLogQueueSize.get() > maxTLogQueueGate ||
			    dataDistributionQueueSize.get() > maxDataDistributionQueueSize ||
			    storageQueueSize.get() > maxStorageServerQueueGate || dataDistributionActive.get() == false ||
			    storageServersRecruiting.get() == true || teamCollectionValid.get() == false) {

				wait( delay( 1.0 ) );
				numSuccesses = 0;
			} else {
				if(++numSuccesses == 3) {
					TraceEvent(("QuietDatabase" + phase + "Done").c_str());
					break;
				}
				else
					wait(delay( 2.0 ) );
			}
		} catch (Error& e) {
			if( e.code() != error_code_actor_cancelled && e.code() != error_code_attribute_not_found && e.code() != error_code_timed_out)
				TraceEvent(("QuietDatabase" + phase + "Error").c_str()).error(e);

			//Client invalid operation occurs if we don't get back a message from one of the servers, often corrected by retrying
			if(e.code() != error_code_attribute_not_found && e.code() != error_code_timed_out)
				throw;

			TraceEvent(("QuietDatabase" + phase + "Retry").c_str()).error(e);
			wait(delay(1.0));
			numSuccesses = 0;
		}
	}

	return Void();
}

Future<Void> quietDatabase( Database const& cx, Reference<AsyncVar<ServerDBInfo>> const& dbInfo, std::string phase, int64_t dataInFlightGate,
	int64_t maxTLogQueueGate, int64_t maxStorageServerQueueGate, int64_t maxDataDistributionQueueSize ) {
	return waitForQuietDatabase(cx, dbInfo, phase, dataInFlightGate, maxTLogQueueGate, maxStorageServerQueueGate, maxDataDistributionQueueSize);
}
