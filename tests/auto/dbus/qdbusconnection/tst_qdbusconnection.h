// Copyright (C) 2016 The Qt Company Ltd.
// Copyright (C) 2016 Intel Corporation.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#ifndef TST_QDBUSCONNECTION_H
#define TST_QDBUSCONNECTION_H

#include <QObject>
#include <QTest>
#include <QTestEventLoop>
#include <QDBusMessage>
#include <QDBusConnection>
#include <QDBusServer>
#include <QDBusVirtualObject>

class BaseObject: public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "local.BaseObject")
public:
    BaseObject(QObject *parent = nullptr) : QObject(parent) { }
public slots:
    void anotherMethod() { }
signals:
    void baseObjectSignal();
};

class MyObject: public BaseObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "local.MyObject")
public slots:
    void method(const QDBusMessage &msg);

public:
    static QString path;
    int callCount;
    MyObject(QObject *parent = nullptr) : BaseObject(parent), callCount(0) {}

signals:
    void myObjectSignal();
};

class MyObjectWithoutInterface: public QObject
{
    Q_OBJECT
public slots:
    void method(const QDBusMessage &msg);

public:
    static QString path;
    static QString interface;
    int callCount;
    MyObjectWithoutInterface(QObject *parent = nullptr) : QObject(parent), callCount(0) {}
};

class SignalReceiver : public QObject
{
    Q_OBJECT
public:
    QString argumentReceived;
    int signalsReceived;
    SignalReceiver() : signalsReceived(0) {}

public slots:
    void oneSlot(const QString &arg) { ++signalsReceived; argumentReceived = arg;}
    void oneSlot() { ++signalsReceived; }
    void exitLoop() { ++signalsReceived; QTestEventLoop::instance().exitLoop(); }
    void secondCallWithCallback();
};

class tst_QDBusConnection: public QObject
{
    Q_OBJECT

public:
    static int hookCallCount;
    tst_QDBusConnection();

public slots:
    void init();
    void cleanup();

private slots:
    void noConnection();
    void connectToBus();
    void connectToPeer();
    void connect();
    void send();
    void sendWithGui();
    void sendAsync();
    void sendSignal();
    void sendSignalToName();
    void sendSignalToOtherName();

    void registerObject_data();
    void registerObject();
    void registerObjectWithInterface_data();
    void registerObjectWithInterface();
    void registerObjectPeer_data();
    void registerObjectPeer();
    void registerObject2();
    void registerObjectPeer2();

    void registerQObjectChildren();
    void registerQObjectChildrenPeer();

    void callSelf();
    void callSelfByAnotherName_data();
    void callSelfByAnotherName();
    void multipleInterfacesInQObject();

    void connectSignal();
    void slotsWithLessParameters();
    void nestedCallWithCallback();

    void serviceRegistrationRaceCondition();

    void registerVirtualObject();
    void callVirtualObject();
    void callVirtualObjectLocal();
    void pendingCallWhenDisconnected();
    void connectionLimit();

    void emptyServerAddress();

    void parentClassSignal();

public:
    QString serviceName() const { return "org.qtproject.Qt.Autotests.QDBusConnection"; }
    bool callMethod(const QDBusConnection &conn, const QString &path);
    bool callMethod(const QDBusConnection &conn, const QString &path, const QString &interface);
    bool callMethodPeer(const QDBusConnection &conn, const QString &path);
};

class QDBusSpy: public QObject
{
    Q_OBJECT
public slots:
    void handlePing(const QString &str) { args.clear(); args << str; }
    void asyncReply(const QDBusMessage &msg) { args = msg.arguments(); }

public:
    QList<QVariant> args;
};

class MyServer : public QDBusServer
{
    Q_OBJECT
public:
    MyServer(QString path) : m_path(path), m_connections()
    {
        connect(this, SIGNAL(newConnection(QDBusConnection)), SLOT(handleConnection(QDBusConnection)));
    }

    bool registerObject(const QDBusConnection& c)
    {
        QDBusConnection conn(c);
        if (!conn.registerObject(m_path, &m_obj, QDBusConnection::ExportAllSlots))
            return false;
        if (!(conn.objectRegisteredAt(m_path) == &m_obj))
            return false;
        return true;
    }

    bool registerObject()
    {
        for (const QString &name : std::as_const(m_connections)) {
            if (!registerObject(QDBusConnection(name)))
                return false;
        }
        return true;
    }

    void unregisterObject()
    {
        for (const QString &name : std::as_const(m_connections)) {
            QDBusConnection c(name);
            c.unregisterObject(m_path);
        }
    }

public slots:
    void handleConnection(const QDBusConnection& c)
    {
        m_connections << c.name();
        QVERIFY(isConnected());
        QVERIFY(c.isConnected());
        QVERIFY(registerObject(c));
        QTestEventLoop::instance().exitLoop();
    }

private:
    MyObject m_obj;
    QString m_path;
    QStringList m_connections;
};

class MyServer2 : public QDBusServer
{
    Q_OBJECT
public:
    MyServer2() : m_conn("none")
    {
        connect(this, SIGNAL(newConnection(QDBusConnection)), SLOT(handleConnection(QDBusConnection)));
    }

    QDBusConnection connection()
    {
        return m_conn;
    }

public slots:
    void handleConnection(const QDBusConnection& c)
    {
        m_conn = c;
        QVERIFY(isConnected());
        QVERIFY(m_conn.isConnected());
        QTestEventLoop::instance().exitLoop();
    }

private:
    MyObject m_obj;
    QDBusConnection m_conn;
};

class TestObject : public QObject
{
Q_OBJECT
public:
    TestObject(QObject *parent = nullptr) : QObject(parent) {}
    ~TestObject() {}

    QString func;

public slots:
    void test0() { func = "test0"; }
    void test1(int i) { func = "test1 " + QString::number(i); }
    int test2() { func = "test2"; return 43; }
    int test3(int i) { func = "test2"; return i + 1; }
};

class RaceConditionSignalWaiter : public QObject
{
    Q_OBJECT
public:
    int count;
    RaceConditionSignalWaiter() : count (0) {}
    virtual ~RaceConditionSignalWaiter() {}

public slots:
    void countUp() { ++count; emit done(); }
signals:
    void done();
};

class VirtualObject: public QDBusVirtualObject
{
    Q_OBJECT
public:
    VirtualObject() :success(true) {}

    QString introspect(const QString & /* path */) const override
    {
        return QString();
    }

    bool handleMessage(const QDBusMessage &message, const QDBusConnection &connection) override {
        ++callCount;
        lastMessage = message;

        if (success) {
            QDBusMessage reply = message.createReply(replyArguments);
            connection.send(reply);
        }
        emit messageReceived(message);
        return success;
    }
signals:
    void messageReceived(const QDBusMessage &message) const;

public:
    mutable QDBusMessage lastMessage;
    QVariantList replyArguments;
    mutable int callCount;
    bool success;
};


#endif // TST_QDBUSCONNECTION_H

