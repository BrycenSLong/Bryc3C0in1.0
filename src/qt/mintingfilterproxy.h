// Copyright (c) 2012-2025 The Brycecoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef Brycecoin_QT_MINTINGFILTERPROXY_H
#define Brycecoin_QT_MINTINGFILTERPROXY_H

#include <QSortFilterProxyModel>

class MintingFilterProxy : public QSortFilterProxyModel
{
    Q_OBJECT
public:
    explicit MintingFilterProxy(QObject *parent = 0);
};

#endif // Brycecoin_QT_MINTINGFILTERPROXY_H
