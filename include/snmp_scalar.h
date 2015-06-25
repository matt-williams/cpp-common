/**
 * @file snmp_scalar.h
 *
 * Project Clearwater - IMS in the Cloud
 * Copyright (C) 2015 Metaswitch Networks Ltd
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

#include <string>

#include "snmp_includes.h"

#ifndef SNMP_SCALAR_H
#define SNMP_SCALAR_H

// This file contains infrastructure for SNMP scalars (single values, not in a table).
//
// To use one, simply create a U32Scalar and modify its `value` object as necessary - changes to
// this will automatically be reflected over SNMP. For example:
//
// SNMP::U32Scalar* cxn_count = new SNMP::U32Scalar("bono_cxn_count",
//                                                  my_oid,
//                                                  OID_LENGTH(my_oid));
// cxn_count.value = 42;

namespace SNMP
{

// Exposes a number as an SNMP Unsigned32.
class U32Scalar
{
public:
  U32Scalar(std::string name,
            oid* oid_param,
            int oidlen):
    value(0),
    _oid(oid_param),
    _oidlen(oidlen)
  {
    netsnmp_register_read_only_ulong_instance(name.c_str(),
                                              _oid,
                                              _oidlen,
                                              &value,
                                              NULL);
  }

  ~U32Scalar()
  {
    // Call into netsnmp to unregister this OID.
    unregister_mib(_oid, _oidlen);
  }

  unsigned long value;
  
private:
  oid* _oid;
  int _oidlen;
};

}
#endif
