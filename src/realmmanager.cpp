/**
 * @file realmmanager.cpp
 *
 * Project Clearwater - IMS in the Cloud
 * Copyright (C) 2013  Metaswitch Networks Ltd
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

#include "realmmanager.h"
#include "utils.h"
#include "cpp_common_pd_definitions.h"

#include <boost/algorithm/string/replace.hpp>

RealmManager::RealmManager(Diameter::Stack* stack,
                           std::string realm,
                           std::string host,
                           int max_peers,
                           DiameterResolver* resolver) :
                           _stack(stack),
                           _realm(realm),
                           _host(host),
                           _max_peers(max_peers),
                           _resolver(resolver),
                           _terminating(false)
{
  pthread_mutex_init(&_lock, NULL);
  pthread_condattr_t cond_attr;
  pthread_condattr_init(&cond_attr);
  pthread_condattr_setclock(&cond_attr, CLOCK_MONOTONIC);
  pthread_cond_init(&_cond, &cond_attr);
  pthread_condattr_destroy(&cond_attr);
}

void RealmManager::start()
{
  using namespace std::placeholders;

  pthread_create(&_thread, NULL, thread_function, this);
  _stack->register_peer_hook_hdlr(std::bind(&RealmManager::peer_connection_cb,
                                            this,
                                            _1,
                                            _2,
                                            _3));
}

RealmManager::~RealmManager()
{
  pthread_mutex_destroy(&_lock);
  pthread_cond_destroy(&_cond);
}

void RealmManager::stop()
{
  pthread_mutex_lock(&_lock);
  _terminating = true;
  pthread_cond_signal(&_cond);
  pthread_mutex_unlock(&_lock);
  pthread_join(_thread, NULL);
  _stack->unregister_peer_hook_hdlr();
}

void RealmManager::peer_connection_cb(bool connection_success,
                                      const std::string& host,
                                      const std::string& realm)
{
  pthread_mutex_lock(&_lock);
  std::map<std::string, Diameter::Peer*>::iterator ii = _peers.find(host);
  if (ii != _peers.end())
  {
    Diameter::Peer* peer = ii->second;
    if (connection_success)
    {
      if (peer->realm().empty() || (peer->realm().compare(realm) == 0))
      {
        TRC_INFO("Successfully connected to %s in realm %s",
                 host.c_str(),
                 realm.c_str());
        peer->set_connected();
      }
      else
      {
        TRC_ERROR("Connected to %s in wrong realm (expected %s, got %s), disconnect",
                  host.c_str(),
                  peer->realm().c_str(),
                  realm.c_str());
        _stack->remove(peer);
        _resolver->blacklist(peer->addr_info());
        delete peer;
        _peers.erase(ii);
      }
    }
    else
    {
      CL_DIAMETER_CONN_ERR.log(host.c_str());
      TRC_ERROR("Failed to connect to %s", host.c_str());
      _resolver->blacklist(peer->addr_info());
      delete peer;
      _peers.erase(ii);

      pthread_cond_signal(&_cond);
    }
  }
  else
  {
    TRC_ERROR("Unexpected host on peer connection callback from freeDiameter: %s",
              host.c_str());
  }

  pthread_mutex_unlock(&_lock);
  return;
}

void* RealmManager::thread_function(void* realm_manager_ptr)
{
  ((RealmManager*)realm_manager_ptr)->thread_function();
  return NULL;
}

// There is a thread running in this function the whole time that
// the program is running. It calls into a function that is responsible
// for managing Diameter connections every time the thread is woken up.
void RealmManager::thread_function()
{
  int ttl = 0;
  struct timespec ts;

  pthread_mutex_lock(&_lock);

  do
  {
    manage_connections(ttl);

    // Call pthread_cond_timedwait to pause the thread until either the
    // TTL of one of the DNS entries expires, we get called by a
    // failure to connect to one of the peers, or the program is
    // terminating. In the latter case we exit the loop and tidy up the
    // connections.
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_sec += ttl;
    pthread_cond_timedwait(&_cond, &_lock, &ts);

  } while (!_terminating);

  // Terminating so remove all peers and tidy up.
  for (std::map<std::string, Diameter::Peer*>::iterator ii = _peers.begin();
       ii != _peers.end();
       ii++)
  {
    _stack->remove(ii->second);
    delete (ii->second);
  }

  // This _peers map contains rubbish at this point, don't use it.
  _peers.clear();

  pthread_mutex_unlock(&_lock);
}

// This function is responsible for managing Diameter connections to a
// given realm.
//
// We maintain several lists in this function. Here we describe the
// flows in this function and how these lists are used. These can be
// cross referenced with the numbered comments below.
//
// 1. Call resolve to perform a DNS resolution on the given realm and
//    populate targets with a list of potential peers to connect to. This
//    also returns a minimum TTL for the DNS entries.
// 2. Create a list of hostnames from the list of targets. This is
//    new_peers and it is used to compare hostnames with existing
//    connections.
// 3. _peers contains a list of the Diameter::Peer objects which
//    represent peers that we have either connected to, or which
//    we are currently trying to connect to. Filter this list on
//    existing connections into connected_peers.
// 4. If we have too many connected_peers (i.e. more than _max_peers),
//    remove some connections. We only remove connections to peers
//    that haven't been returned by the resolve function on this run
//    through.
// 5. Connect to any peers in the list of targets that we aren't already
//    connected to. This is how we can end up with too many connections
//    (as per step 4). We don't tear down any connections until we're sure
//    we have _max_peers connections. The only exception to this is
//    when resolve contains fewer than _max_peers entries which means we
//    sure we should have fewer than _max_peers connections.
// 6. Tell the stack the number of peers we are aware of, and the number of
//    peers we're connected to. This is so that the stack can raise appropriate
//    logs when we have no connections and routing messages inevitably fails.
//
// On the first run through this function, a lot of this processing is
// irrelevant since we just get a list of targets and try to connect to
// them.
void RealmManager::manage_connections(int& ttl)
{
  std::vector<AddrInfo> targets;
  std::vector<std::string> new_peers;
  std::vector<Diameter::Peer*> connected_peers;
  bool ret;

  // 1.
  _resolver->resolve(_realm, _host, _max_peers, targets, ttl);

  // We impose sensible max and min values for the TTL.
  ttl = std::max(5, ttl);
  ttl = std::min(300, ttl);

  // 2.
  for (std::vector<AddrInfo>::iterator ii = targets.begin();
       ii != targets.end();
       ii++)
  {
    new_peers.push_back(Utils::ip_addr_to_arpa((*ii).address));
  }

  // 3.
  for (std::map<std::string, Diameter::Peer*>::iterator ii = _peers.begin();
       ii != _peers.end();
       ii++)
  {
    if ((ii->second)->connected())
    {
      connected_peers.push_back(ii->second);
    }
  }

  // 4.
  for (std::vector<Diameter::Peer*>::iterator ii = connected_peers.begin();
       (ii != connected_peers.end()) && (((int)connected_peers.size() > _max_peers) || ((int)new_peers.size() < _max_peers));
      )
  {
    if (std::find(new_peers.begin(), new_peers.end(), (*ii)->host()) == new_peers.end())
    {
      Diameter::Peer* peer = *ii;
      TRC_DEBUG("Removing peer: %s", peer->host().c_str());
      ii = connected_peers.erase(ii);
      std::map<std::string, Diameter::Peer*>::iterator jj = _peers.find(peer->host());
      if (jj != _peers.end())
      {
        _peers.erase(jj);
      }
      _stack->remove(peer);
      delete peer;
    }
    else
    {
      ii++;
    }
  }

  // 5.
  int zombies = 0;
  for (std::vector<AddrInfo>::iterator ii = targets.begin();
       ii != targets.end();
       ii++)
  {
    std::string hostname = Utils::ip_addr_to_arpa((*ii).address);
    bool found = false;

    // Check whether this new target is already in our list of _peers.
    std::map<std::string, Diameter::Peer*>::iterator jj = _peers.find(hostname);
    if (jj != _peers.end())
    {
      found = true;
    }

    // If it isn't, add it.
    if (!found)
    {
      Diameter::Peer* peer = new Diameter::Peer(*ii, hostname, _realm, 0);
      TRC_DEBUG("Adding peer: %s", hostname.c_str());
      ret = _stack->add(peer);
      if (ret)
      {
        _peers[hostname] = peer;
      }
      else
      {
        TRC_DEBUG("Peer already exists: %s", hostname.c_str());
        delete peer;

        // If the add failed, it means that, while RealmManager and
        // DiameterStack don't have this peer in their list, it still exists in
        // freeDiameter.  This can happen for "zombie" peers whose garbage
        // collection hasn't happened yet.
        //
        // Increment our "zombie" count.
        zombies++;
      }
    }
  }

  // 6. Tell the stack the number of peers we're currently managing (including
  // any peers we are waiting to be able to add, but can't because of delayed
  // zombie cleanup in freeDiameter), and the number of peers we're actually
  // connected to.
  _stack->peer_count(_peers.size() + zombies, connected_peers.size());
}
