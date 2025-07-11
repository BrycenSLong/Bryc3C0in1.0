// Copyright (c) 2009-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_CLIENTVERSION_H
#define BITCOIN_CLIENTVERSION_H

#include <util/macros.h>

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif //HAVE_CONFIG_H

// Check that required client information is defined
#if !defined(BRYC3C0IN_VERSION_MAJOR) || !defined(BRYC3C0IN_VERSION_MINOR) || !defined(BRYC3C0IN_VERSION_REVISION) || !defined(BRYC3C0IN_VERSION_BUILD)
#error Client version information missing: version is not defined by bitcoin-config.h or in any other way
#endif

//! Copyright string used in Windows .rc files
#define COPYRIGHT_STR "2009-" STRINGIZE(COPYRIGHT_YEAR) " " COPYRIGHT_HOLDERS_FINAL

/**
 * bitcoind-res.rc includes this file, but it cannot cope with real c++ code.
 * WINDRES_PREPROC is defined to indicate that its pre-processor is running.
 * Anything other than a define should be guarded below.
 */

#if !defined(WINDRES_PREPROC)

#include <string>
#include <vector>

static const int CLIENT_VERSION =
                             10000 * CLIENT_VERSION_MAJOR
                         +     100 * CLIENT_VERSION_MINOR
                         +       1 * CLIENT_VERSION_BUILD;

// note: bryc3c0in version is used for display purpose AND to accept alerts
static const int BRYC3C0IN_VERSION =
                           1000000 * BRYC3C0IN_VERSION_MAJOR
                         +   10000 * BRYC3C0IN_VERSION_MINOR
                         +     100 * BRYC3C0IN_VERSION_REVISION
                         +       1 * BRYC3C0IN_VERSION_BUILD;

extern const std::string CLIENT_NAME;


std::string FormatFullVersion();
std::string FormatSubVersion(const std::string& name, int nClientVersion, const std::vector<std::string>& comments);

std::string CopyrightHolders(const std::string& strPrefix);

/** Returns licensing information (for -version) */
std::string LicenseInfo();

#endif // WINDRES_PREPROC

#endif // BITCOIN_CLIENTVERSION_H
