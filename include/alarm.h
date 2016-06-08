/**
 * @file alarm.h
 *
 * Project Clearwater - IMS in the Cloud
 * Copyright (C) 2014  Metaswitch Networks Ltd
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version, along with the "Special Exception" for use of
 * the program along with SSL, set forth below. This program is distributed
 * in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details. You should have received a copy of the GNU General Public
 * License along with this program.  If not, see
 * <http://www.gnu.org/licenses/>.
 *
 * The author can be reached by email at clearwater@metaswitch.com or by
 * post at Metaswitch Networks Ltd, 100 Church St, Enfield EN2 6BQ, UK
 *
 * Special Exception
 * Metaswitch Networks Ltd  grants you permission to copy, modify,
 * propagate, and distribute a work formed by combining OpenSSL with The
 * Software, or a work derivative of such a combination, even if such
 * copying, modification, propagation, or distribution would otherwise
 * violate the terms of the GPL. You must comply with the GPL in all
 * respects for all of the code used other than OpenSSL.
 * "OpenSSL" means OpenSSL toolkit software distributed by the OpenSSL
 * Project and licensed under the OpenSSL Licenses, or a work based on such
 * software and licensed under the OpenSSL Licenses.
 * "OpenSSL Licenses" means the OpenSSL License and Original SSLeay License
 * under which the OpenSSL Project distributes the OpenSSL toolkit software,
 * as those licenses appear in the file LICENSE-OPENSSL.
 */

#ifndef ALARM_H__
#define ALARM_H__

#include <pthread.h>

#include <string>
#include <vector>
#include <atomic>
#include <memory>

#include "alarmdefinition.h"
#include "eventq.h"

#ifdef UNIT_TEST
#include "pthread_cond_var_helper.h"
#else
#include "cond_var.h"
#endif

class AlarmManager;

/// @class AlarmReqAgent
///
/// Class which provides an agent thead to accept queued alarm requests from
/// clients and forward them via ZMQ to snmpd (which will actually generate the
/// inform message(s)).

class AlarmReqAgent
{
public:
  AlarmReqAgent();
  ~AlarmReqAgent();

  enum
  {
    MAX_Q_DEPTH = 100
  };

  /// Initialize ZMQ context and start agent thread.
  bool start();

  /// Gracefully stop the agent thread and remove ZMQ context.
  void stop();

  /// Queue an alarm request to be forwarded to snmpd.
  void alarm_request(std::vector<std::string> req);

private:
  enum
  {
    ZMQ_PORT = 6664,
    MAX_REPLY_LEN = 16
  };

  static void* agent_thread(void* alarm_req_agent);

  bool zmq_init_ctx();
  bool zmq_init_sck();

  void zmq_clean_ctx();
  void zmq_clean_sck();

  void agent();

  pthread_t _thread;

  pthread_mutex_t _start_mutex;
  pthread_cond_t  _start_cond;

  void* _ctx;
  void* _sck;

  eventq<std::vector<std::string> >* _req_q;
};

/// @class AlarmState
///
/// Provides the basic interface for updating an SNMP alarm's state.

class AlarmState
{
public:
  AlarmState(AlarmReqAgent* alarm_req_agent,
             const std::string& issuer,
             const int index,
             AlarmDef::Severity severity);

  /// Queue request to update the alarm identified by index, to the
  /// state associated with severity, for the specified issuer.
  void issue();

  std::string& get_issuer() {return _issuer;}
  std::string& get_identifier() {return _identifier;}

  /// @enum AlarmCondition
  ///
  /// Enum for the three possible general states an alarm can be in.
  /// i.e. Not yet set, raised or cleared.
  enum AlarmCondition
  {
    // The state in which all alarms start, indicating that
    // no state has been explicitly raised
    UNKNOWN,
    // The alarm has been explicitly cleared
    CLEARED,
    // The alarm has been raised at any severity other than cleared
    ALARMED
  };

private:
  AlarmReqAgent* _alarm_req_agent;
  std::string _issuer;
  std::string _identifier;
};

/// @class BaseAlarm
/// 
/// Superclass for alarms that allows us to split different types of
/// alarms into subclasses. Those alarms which only have one possible raised
/// state will be constructed by subclass Alarm. Those alarms which have two or
/// more possible raised states will be constructed by subclass
/// MultiStateAlarm.

class BaseAlarm
{
public:
  /// Queues a request to generate an alarm state change corresponding to the
  /// CLEARED severity.
  virtual void clear();

  /// Uses the _last_state_raised member variable to re-raise the latest state
  /// of the alarm.
  void reraise_last_state();
  
  /// Returns the current state of the alarm as one of UNKNOWN, CLEARED, or ALARMED.
  virtual AlarmState::AlarmCondition get_alarm_state();

  // If an alarm is currently in a different state to the one we wish to raise
  // the alarm in, we raise the alarm and update _last_state_raised.
  void switch_to_state(AlarmState* new_state);

protected:
  BaseAlarm(AlarmManager* alarm_manager,
            const std::string& issuer,
            const int index);

  const int _index;
  AlarmState _clear_state;
  
  // Keeps track of the latest state of each alarm that has been raised. If the
  // alarm has just been cleared this would be the corresponding _clear_state
  // for the alarm.
  AlarmState* _last_state_raised;
};

/// @class AlarmReRaiser
///
/// Class responsible for calling BaseAlarm's reraise_latest_state
/// function on each alarm every thirty seconds.
class AlarmReRaiser
{
public:
  AlarmReRaiser();
  ~AlarmReRaiser();

  // Tell the Alarm Manager about an alarm
  void register_alarm(BaseAlarm* alarm); 

private:
  bool _terminated;

  // Static function called by the reraising alarms thread. This simply calls
  // the 'reraise_alarms' member method
  static void* reraise_alarms_function(void* data);

  // This runs on a thread (defined below) and iterates over _alarm_list
  // every 30 seconds. For each alarm it calls the reraise_last_state method.
  void reraise_alarms();

  // This is used for storing all of the BaseAlarm objects as they get
  // registered.
  std::vector<BaseAlarm*> _alarm_list;

  // Defines a lock and condition variable to protect the reraising alarms
  // thread.
  pthread_mutex_t _lock;
#ifdef UNIT_TEST
  MockPThreadCondVar* _condition;
#else
  CondVar* _condition;
#endif
  pthread_t _reraising_alarms_thread;
};

/// @class AlarmManager
///
/// Class responsible for calling BaseAlarm's reraise_latest_state
/// function on each alarm every thirty seconds.
class AlarmManager
{
public:
  AlarmManager() :
    _alarm_req_agent(new AlarmReqAgent()),
    _alarm_re_raiser(new AlarmReRaiser())
  {}

  ~AlarmManager()
  {
    delete _alarm_re_raiser; _alarm_re_raiser = NULL;
    delete _alarm_req_agent; _alarm_req_agent = NULL;
  }

  bool start() { return _alarm_req_agent->start(); }
  void stop() { _alarm_req_agent->stop(); }

  AlarmReqAgent* alarm_req_agent() { return _alarm_req_agent; }
  AlarmReRaiser* alarm_re_raiser() { return _alarm_re_raiser; }

private:
  AlarmReqAgent* _alarm_req_agent;
  AlarmReRaiser* _alarm_re_raiser;
};

/// @class Alarm
///
/// Encapsulates an alarm's only active state with its associated alarm clear state.
/// Used to manage the reporting of a fault condition, and subsequent clear
/// of said condition.

class Alarm: public BaseAlarm
{
public:
  Alarm(AlarmManager* alarm_manager,
        const std::string& issuer,
        const int index,
        AlarmDef::Severity severity);

  virtual ~Alarm() {}

  /// Queues a request to generate an alarm state change corresponding to the
  /// non-CLEARED severity if a state change for the CLEARED severity was
  /// previously requested via clear().
  virtual void set();

  /// Returns the index of this alarm.
  int index() const {return _index;}
  
private:
  AlarmState _set_state;
};

/// @class MultiStateAlarm
///
/// Encapsulates an alarm's two or more active states with its associated clear
/// state. Used to manage the reporting of a fault condition, and subsequent
/// clear of said condition. We further subclass this on a per-alarm basis, to
/// give an alarm visibility of the protected raising functions cordesponding to that
/// alarm's possible raised states. This would stop a user raising alarm at a
/// state which does not exist for that alarm.

class MultiStateAlarm: public BaseAlarm
{
public:
  MultiStateAlarm(AlarmManager* alarm_manager,
                  const std::string& issuer,
                  const int index);

  virtual ~MultiStateAlarm() {}
  
protected:
  /// These raise the alarm with the specified severity.
  void set_indeterminate();
  void set_warning();
  void set_minor();
  void set_major();
  void set_critical();

private:
  AlarmState _indeterminate_state;
  AlarmState _warning_state;
  AlarmState _minor_state;
  AlarmState _major_state;
  AlarmState _critical_state;
};

#endif
