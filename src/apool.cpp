/* 
 * SPDX-FileCopyrightText: (C) 2020-2021 Daniel Nicoletti <dantti12@gmail.com>
 * SPDX-License-Identifier: MIT
 */

#include "apool.h"
#include "adriver.h"
#include "adriverfactory.h"

#include <QPointer>
#include <QQueue>
#include <QObject>
#include <QLoggingCategory>

Q_LOGGING_CATEGORY(ASQL_POOL, "asql.pool", QtInfoMsg)

struct APoolQueuedClient {
    std::function<void (ADatabase &)> cb;
    QPointer<QObject> receiver;
    bool checkReceiver;
};

struct APoolInternal {
    std::shared_ptr<ADriverFactory> driverFactory;
    QVector<ADriver *> pool;
    QQueue<APoolQueuedClient> connectionQueue;
    std::function<void (ADatabase &)> setupCb;
    std::function<void (ADatabase &)> reuseCb;
    int maxIdleConnections = 1;
    int maximuConnections = 0;
    int connectionCount = 0;
};

static thread_local QHash<QString, APoolInternal> m_connectionPool;

const char *APool::defaultPool = const_cast<char *>("asql_default_pool");

void APool::addDatabase(const std::shared_ptr<ADriverFactory> &factory, const QString &poolName)
{
    create(factory, poolName);
}

void APool::create(const std::shared_ptr<ADriverFactory> &factory, const QString &poolName)
{
    if (!m_connectionPool.contains(poolName)) {
        APoolInternal pool;
        pool.driverFactory = factory;
        m_connectionPool.insert(poolName, pool);
    } else {
        qWarning(ASQL_POOL) << "Ignoring addDatabase, connectionName already available" << poolName;
    }
}

void APool::remove(const QString &poolName)
{
    m_connectionPool.remove(poolName);
}

void APool::pushDatabaseBack(const QString &connectionName, ADriver *driver)
{
    auto it = m_connectionPool.find(connectionName);
    if (it != m_connectionPool.end()) {
        APoolInternal &iPool = it.value();
        if (driver->state() == ADatabase::State::Disconnected) {
            qDebug(ASQL_POOL) << "Deleting database connection as is not open" << driver->isOpen();
            delete driver;
            --iPool.connectionCount;
            return;
        }

        // Check for waiting clients
        while (!iPool.connectionQueue.isEmpty()) {
            APoolQueuedClient client = iPool.connectionQueue.dequeue();
            if ((client.checkReceiver && client.receiver.isNull()) || !client.cb) {
                continue;
            }

            ADatabase db;
            db.d = std::shared_ptr<ADriver>(driver, [connectionName] (ADriver *driver) {
                    pushDatabaseBack(connectionName, driver);
            });
            client.cb(db);
            return;
        }

        if (iPool.pool.size() >= iPool.maxIdleConnections) {
            qDebug(ASQL_POOL) << "Deleting database connection due max idle connections" << iPool.maxIdleConnections << iPool.pool.size();
            delete driver;
            --iPool.connectionCount;
        } else {
            qDebug(ASQL_POOL) << "Returning database connection to pool" << connectionName << driver;
            iPool.pool.push_back(driver);
        }
    } else {
        delete driver;
    }
}

ADatabase APool::database(const QString &poolName)
{
    ADatabase db;
    auto it = m_connectionPool.find(poolName);
    if (it != m_connectionPool.end()) {
        APoolInternal &iPool = it.value();
        if (iPool.pool.empty()) {
            if (iPool.maximuConnections && iPool.connectionCount >= iPool.maximuConnections) {
                qWarning(ASQL_POOL) << "Maximum number of connections reached" << poolName << iPool.connectionCount << iPool.maximuConnections;
            } else {
                ++iPool.connectionCount;
                auto driver = iPool.driverFactory->createRawDriver();
                qDebug(ASQL_POOL) << "Creating a database connection for pool" << poolName << driver;
                db.d = std::shared_ptr<ADriver>(driver, [poolName] (ADriver *driver) {
                    pushDatabaseBack(poolName, driver);
                });

                if (iPool.setupCb) {
                    iPool.setupCb(db);
                }
            }
        } else {
            qDebug(ASQL_POOL) << "Reusing a database connection from pool" << poolName;
            ADriver *driver = iPool.pool.takeLast();
            db.d = std::shared_ptr<ADriver>(driver, [poolName] (ADriver *driver) {
                pushDatabaseBack(poolName, driver);
            });

            if (iPool.reuseCb) {
                iPool.reuseCb(db);
            }
        }
    } else {
        qCritical(ASQL_POOL) << "Database pool NOT FOUND" << poolName;
    }
    db.open();
    return db;
}

int APool::currentConnections(const QString &poolName)
{
    auto it = m_connectionPool.find(poolName);
    if (it != m_connectionPool.end()) {
        return it.value().connectionCount;
    }
    return 0;
}

void APool::database(std::function<void (ADatabase &)> cb, QObject *receiver, const QString &poolName)
{
    ADatabase db;
    auto it = m_connectionPool.find(poolName);
    if (it != m_connectionPool.end()) {
        APoolInternal &iPool = it.value();
        if (iPool.pool.empty()) {
            if (iPool.maximuConnections && iPool.connectionCount >= iPool.maximuConnections) {
                qInfo(ASQL_POOL) << "Maximum number of connections reached, queuing" << poolName << iPool.connectionCount << iPool.maximuConnections;
                APoolQueuedClient queued;
                queued.cb = cb;
                queued.receiver = receiver;
                queued.checkReceiver = receiver;
                iPool.connectionQueue.enqueue(queued);
                return;
            }
            ++iPool.connectionCount;
            qDebug(ASQL_POOL) << "Creating a database connection for pool" << poolName;
            db.d = std::shared_ptr<ADriver>(iPool.driverFactory->createRawDriver(), [poolName] (ADriver *driver) {
                    pushDatabaseBack(poolName, driver);
            });

            if (iPool.setupCb) {
                iPool.setupCb(db);
            }
        } else {
            qDebug(ASQL_POOL) << "Reusing a database connection from pool" << poolName;
            ADriver *priv = iPool.pool.takeLast();
            db.d = std::shared_ptr<ADriver>(priv, [poolName] (ADriver *driver) {
                    pushDatabaseBack(poolName, driver);
            });

            if (iPool.reuseCb) {
                iPool.reuseCb(db);
            }
        }
    } else {
        qCritical(ASQL_POOL) << "Database pool NOT FOUND" << poolName;
    }
    db.open();

    if (cb) {
        cb(db);
    }
}

void APool::setMaxIdleConnections(int max, const QString &poolName)
{
    auto it = m_connectionPool.find(poolName);
    if (it != m_connectionPool.end()) {
        it.value().maxIdleConnections = max;
    } else {
        qCritical(ASQL_POOL) << "Failed to set maximum idle connections: Database pool NOT FOUND" << poolName;
    }
}

void APool::setMaxConnections(int max, const QString &poolName)
{
    auto it = m_connectionPool.find(poolName);
    if (it != m_connectionPool.end()) {
        it.value().maximuConnections = max;
    } else {
        qCritical(ASQL_POOL) << "Failed to set maximum connections: Database pool NOT FOUND" << poolName;
    }
}

void APool::setSetupCallback(std::function<void (ADatabase &)> cb, const QString &poolName)
{
    auto it = m_connectionPool.find(poolName);
    if (it != m_connectionPool.end()) {
        it.value().setupCb = cb;
    } else {
        qCritical(ASQL_POOL) << "Failed to set maximum connections: Database pool NOT FOUND" << poolName;
    }
}

void APool::setReuseCallback(std::function<void (ADatabase &)> cb, const QString &poolName)
{
    auto it = m_connectionPool.find(poolName);
    if (it != m_connectionPool.end()) {
        it.value().reuseCb = cb;
    } else {
        qCritical(ASQL_POOL) << "Failed to set maximum connections: Database pool NOT FOUND" << poolName;
    }
}

void APool::setDatabaseMaxIdleConnections(int max, const QString &poolName)
{
    setMaxIdleConnections(max, poolName);
}

void APool::setDatabaseMaximumConnections(int max, const QString &poolName)
{
    setMaxConnections(max, poolName);
}
