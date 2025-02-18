// Copyright (C) 2016 The Qt Company Ltd.
// Copyright (C) 2016 Intel Corporation.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "qdbusabstractinterface.h"
#include "qdbusabstractinterface_p.h"

#include <qcoreapplication.h>
#include <qthread.h>

#include "qdbusargument.h"
#include "qdbuspendingcall.h"
#include "qdbusmessage_p.h"
#include "qdbusmetaobject_p.h"
#include "qdbusmetatype_p.h"
#include "qdbusservicewatcher.h"
#include "qdbusutil_p.h"

#include <qdebug.h>

#ifndef QT_NO_DBUS

QT_BEGIN_NAMESPACE

using namespace Qt::StringLiterals;

namespace {
// ### Qt6: change to a regular QEvent (customEvent)
// We need to use a QMetaCallEvent here because we can't override customEvent() in
// Qt 5. Since QDBusAbstractInterface is meant to be derived from, the vtables of
// classes in generated code will have a pointer to QObject::customEvent instead
// of to QDBusAbstractInterface::customEvent.
// See solution in Patch Set 1 of this change in the Qt Gerrit servers.
// (https://codereview.qt-project.org/#/c/126384/1)
class DisconnectRelayEvent : public QAbstractMetaCallEvent
{
public:
    DisconnectRelayEvent(QObject *sender, const QMetaMethod &m)
        : QAbstractMetaCallEvent(sender, m.methodIndex())
    {}

    void placeMetaCall(QObject *object) override
    {
        QDBusAbstractInterface *iface = static_cast<QDBusAbstractInterface *>(object);
        QDBusAbstractInterfacePrivate::finishDisconnectNotify(iface, signalId());
    }
};
}

static QDBusError checkIfValid(const QString &service, const QString &path,
                               const QString &interface, bool isDynamic, bool isPeer)
{
    // We should be throwing exceptions here... oh well
    QDBusError error;

    // dynamic interfaces (QDBusInterface) can have empty interfaces, but not service and object paths
    // non-dynamic is the opposite: service and object paths can be empty, but not the interface
    if (!isDynamic) {
        // use assertion here because this should never happen, at all
        Q_ASSERT_X(!interface.isEmpty(), "QDBusAbstractInterface", "Interface name cannot be empty");
    }
    if (!QDBusUtil::checkBusName(service, (isDynamic && !isPeer) ? QDBusUtil::EmptyNotAllowed : QDBusUtil::EmptyAllowed, &error))
        return error;
    if (!QDBusUtil::checkObjectPath(path, isDynamic ? QDBusUtil::EmptyNotAllowed : QDBusUtil::EmptyAllowed, &error))
        return error;
    if (!QDBusUtil::checkInterfaceName(interface, QDBusUtil::EmptyAllowed, &error))
        return error;

    // no error
    return QDBusError();
}

QDBusAbstractInterfacePrivate::QDBusAbstractInterfacePrivate(const QString &serv,
                                                             const QString &p,
                                                             const QString &iface,
                                                             const QDBusConnection& con,
                                                             bool isDynamic)
    : connection(con), service(serv), path(p), interface(iface),
      lastError(checkIfValid(serv, p, iface, isDynamic, (connectionPrivate() &&
                                                         connectionPrivate()->mode == QDBusConnectionPrivate::PeerMode))),
      timeout(-1),
      interactiveAuthorizationAllowed(false),
      isValid(!lastError.isValid())
{
    if (!isValid)
        return;

    if (!connection.isConnected()) {
        lastError = QDBusError(QDBusError::Disconnected,
                               QDBusUtil::disconnectedErrorMessage());
    }
}

void QDBusAbstractInterfacePrivate::initOwnerTracking()
{
    if (!isValid || !connection.isConnected() || !connectionPrivate()->shouldWatchService(service))
        return;

    QObject::connect(new QDBusServiceWatcher(service, connection, QDBusServiceWatcher::WatchForOwnerChange, q_func()),
                     SIGNAL(serviceOwnerChanged(QString,QString,QString)),
                     q_func(), SLOT(_q_serviceOwnerChanged(QString,QString,QString)));

    currentOwner = connectionPrivate()->getNameOwner(service);
    if (currentOwner.isEmpty())
        lastError = connectionPrivate()->lastError;
}

bool QDBusAbstractInterfacePrivate::canMakeCalls() const
{
    // recheck only if we have a wildcard (i.e. empty) service or path
    // if any are empty, set the error message according to QDBusUtil
    if (service.isEmpty() && connectionPrivate()->mode != QDBusConnectionPrivate::PeerMode)
        return QDBusUtil::checkBusName(service, QDBusUtil::EmptyNotAllowed, &lastError);
    if (path.isEmpty())
        return QDBusUtil::checkObjectPath(path, QDBusUtil::EmptyNotAllowed, &lastError);
    return true;
}

bool QDBusAbstractInterfacePrivate::property(const QMetaProperty &mp, void *returnValuePtr) const
{
    if (!isValid || !canMakeCalls())   // can't make calls
        return false;

    QMetaType type = mp.metaType();
    // is this metatype registered?
    const char *expectedSignature = "";
    if (type.id() != QMetaType::QVariant) {
        expectedSignature = QDBusMetaType::typeToSignature(type);
        if (expectedSignature == nullptr) {
            qWarning("QDBusAbstractInterface: type %s must be registered with Qt D-Bus before it can be "
                     "used to read property %s.%s",
                     mp.typeName(), qPrintable(interface), mp.name());
            lastError = QDBusError(QDBusError::Failed, "Unregistered type %1 cannot be handled"_L1
                                   .arg(QLatin1StringView(mp.typeName())));
            return false;
        }
    }

    // try to read this property
    QDBusMessage msg = QDBusMessage::createMethodCall(service, path,
                                                      QDBusUtil::dbusInterfaceProperties(),
                                                      QStringLiteral("Get"));
    QDBusMessagePrivate::setParametersValidated(msg, true);
    msg << interface << QString::fromUtf8(mp.name());
    QDBusMessage reply = connection.call(msg, QDBus::Block, timeout);

    if (reply.type() != QDBusMessage::ReplyMessage) {
        lastError = QDBusError(reply);
        return false;
    }
    if (reply.signature() != "v"_L1) {
        QString errmsg =
                "Invalid signature '%1' in return from call to " DBUS_INTERFACE_PROPERTIES ""_L1;
        lastError = QDBusError(QDBusError::InvalidSignature, std::move(errmsg).arg(reply.signature()));
        return false;
    }

    QByteArray foundSignature;
    const char *foundType = nullptr;
    QVariant value = qvariant_cast<QDBusVariant>(reply.arguments().at(0)).variant();

    if (value.metaType() == type || type.id() == QMetaType::QVariant
        || (expectedSignature[0] == 'v' && expectedSignature[1] == '\0')) {
        // simple match
        if (type.id() == QMetaType::QVariant) {
            *reinterpret_cast<QVariant*>(returnValuePtr) = value;
        } else {
            QMetaType(type).destruct(returnValuePtr);
            QMetaType(type).construct(returnValuePtr, value.constData());
        }
        return true;
    }

    if (value.metaType() == QMetaType::fromType<QDBusArgument>()) {
        QDBusArgument arg = qvariant_cast<QDBusArgument>(value);

        foundType = "user type";
        foundSignature = arg.currentSignature().toLatin1();
        if (foundSignature == expectedSignature) {
            // signatures match, we can demarshall
            return QDBusMetaType::demarshall(arg, QMetaType(type), returnValuePtr);
        }
    } else {
        foundType = value.typeName();
        foundSignature = QDBusMetaType::typeToSignature(value.metaType());
    }

    // there was an error...
    const auto errmsg = "Unexpected '%1' (%2) when retrieving property '%3.%4' "
                        "(expected type '%5' (%6))"_L1;
    lastError = QDBusError(QDBusError::InvalidSignature,
                           errmsg.arg(QLatin1StringView(foundType),
                                      QLatin1StringView(foundSignature),
                                      interface,
                                      QLatin1StringView(mp.name()),
                                      QLatin1StringView(mp.typeName()),
                                      QLatin1StringView(expectedSignature)));
    return false;
}

bool QDBusAbstractInterfacePrivate::setProperty(const QMetaProperty &mp, const QVariant &value)
{
    if (!isValid || !canMakeCalls())    // can't make calls
        return false;

    // send the value
    QDBusMessage msg = QDBusMessage::createMethodCall(service, path,
                                                      QDBusUtil::dbusInterfaceProperties(),
                                                      QStringLiteral("Set"));
    QDBusMessagePrivate::setParametersValidated(msg, true);
    msg << interface << QString::fromUtf8(mp.name()) << QVariant::fromValue(QDBusVariant(value));
    QDBusMessage reply = connection.call(msg, QDBus::Block, timeout);

    if (reply.type() != QDBusMessage::ReplyMessage) {
        lastError = QDBusError(reply);
        return false;
    }
    return true;
}

void QDBusAbstractInterfacePrivate::_q_serviceOwnerChanged(const QString &name,
                                                           const QString &oldOwner,
                                                           const QString &newOwner)
{
    Q_UNUSED(oldOwner);
    //qDebug() << "QDBusAbstractInterfacePrivate serviceOwnerChanged" << name << oldOwner << newOwner;
    Q_ASSERT(name == service);
    currentOwner = newOwner;
}

QDBusAbstractInterfaceBase::QDBusAbstractInterfaceBase(QDBusAbstractInterfacePrivate &d, QObject *parent)
    : QObject(d, parent)
{
}

int QDBusAbstractInterfaceBase::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    int saved_id = _id;
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;

    if (_c == QMetaObject::ReadProperty || _c == QMetaObject::WriteProperty) {
        QMetaProperty mp = metaObject()->property(saved_id);
        int &status = *reinterpret_cast<int *>(_a[2]);

        if (_c == QMetaObject::WriteProperty) {
            QVariant value;
            if (mp.metaType() == QMetaType::fromType<QDBusVariant>())
                value = reinterpret_cast<const QDBusVariant*>(_a[0])->variant();
            else
                value = QVariant(mp.metaType(), _a[0]);
            status = d_func()->setProperty(mp, value) ? 1 : 0;
        } else {
            bool readStatus = d_func()->property(mp, _a[0]);
            // Caller supports QVariant returns? Then we can also report errors
            // by storing an invalid variant.
            if (!readStatus && _a[1]) {
                status = 0;
                reinterpret_cast<QVariant*>(_a[1])->clear();
            }
        }
        _id = -1;
    }
    return _id;
}

/*!
    \class QDBusAbstractInterface
    \inmodule QtDBus
    \since 4.2

    \brief The QDBusAbstractInterface class is the base class for all D-Bus interfaces in the Qt D-Bus binding, allowing access to remote interfaces.

    Generated-code classes also derive from QDBusAbstractInterface,
    all methods described here are also valid for generated-code
    classes. In addition to those described here, generated-code
    classes provide member functions for the remote methods, which
    allow for compile-time checking of the correct parameters and
    return values, as well as property type-matching and signal
    parameter-matching.

    \sa {qdbusxml2cpp.html}{The QDBus compiler}, QDBusInterface
*/

/*!
    \internal
    This is the constructor called from QDBusInterface::QDBusInterface.
*/
QDBusAbstractInterface::QDBusAbstractInterface(QDBusAbstractInterfacePrivate &d, QObject *parent)
    : QDBusAbstractInterfaceBase(d, parent)
{
    d.initOwnerTracking();
}

/*!
    \internal
    This is the constructor called from static classes derived from
    QDBusAbstractInterface (i.e., those generated by dbusxml2cpp).
*/
QDBusAbstractInterface::QDBusAbstractInterface(const QString &service, const QString &path,
                                               const char *interface, const QDBusConnection &con,
                                               QObject *parent)
    : QDBusAbstractInterfaceBase(*new QDBusAbstractInterfacePrivate(service, path, QString::fromLatin1(interface),
                                                 con, false), parent)
{
    // keep track of the service owner
    d_func()->initOwnerTracking();
}

/*!
    Releases this object's resources.
*/
QDBusAbstractInterface::~QDBusAbstractInterface()
{
}

/*!
    Returns \c true if this is a valid reference to a remote object. It returns \c false if
    there was an error during the creation of this interface (for instance, if the remote
    application does not exist).

    Note: when dealing with remote objects, it is not always possible to determine if it
    exists when creating a QDBusInterface.
*/
bool QDBusAbstractInterface::isValid() const
{
    Q_D(const QDBusAbstractInterface);
    /* We don't retrieve the owner name for peer connections */
    if (d->connectionPrivate() && d->connectionPrivate()->mode == QDBusConnectionPrivate::PeerMode) {
        return d->isValid;
    } else {
        return !d->currentOwner.isEmpty();
    }
}

/*!
    Returns the connection this interface is associated with.
*/
QDBusConnection QDBusAbstractInterface::connection() const
{
    return d_func()->connection;
}

/*!
    Returns the name of the service this interface is associated with.
*/
QString QDBusAbstractInterface::service() const
{
    return d_func()->service;
}

/*!
    Returns the object path that this interface is associated with.
*/
QString QDBusAbstractInterface::path() const
{
    return d_func()->path;
}

/*!
    Returns the name of this interface.
*/
QString QDBusAbstractInterface::interface() const
{
    return d_func()->interface;
}

/*!
    Returns the error the last operation produced, or an invalid error if the last operation did not
    produce an error.
*/
QDBusError QDBusAbstractInterface::lastError() const
{
    return d_func()->lastError;
}

/*!
    Sets the timeout in milliseconds for all future DBus calls to \a timeout.
    -1 means the default DBus timeout (usually 25 seconds).

    \since 4.8
*/
void QDBusAbstractInterface::setTimeout(int timeout)
{
    d_func()->timeout = timeout;
}

/*!
    Returns the current value of the timeout in milliseconds.
    -1 means the default DBus timeout (usually 25 seconds).

    \since 4.8
*/
int QDBusAbstractInterface::timeout() const
{
    return d_func()->timeout;
}

/*!
    Configures whether, for asynchronous calls, the caller
    is prepared to wait for interactive authorization.

    If \a enable is set to \c true, the D-Bus messages generated for
    asynchronous calls via this interface will set the
    \c ALLOW_INTERACTIVE_AUTHORIZATION flag.

    This flag is only useful when unprivileged code calls a more privileged
    method call, and an authorization framework is deployed that allows
    possibly interactive authorization.

    The default is \c false.

    \since 6.7
    \sa QDBusMessage::setInteractiveAuthorizationAllowed(),
        interactiveAuthorizationAllowed()
*/
void QDBusAbstractInterface::setInteractiveAuthorizationAllowed(bool enable)
{
    d_func()->interactiveAuthorizationAllowed = enable;
}

/*!
    Returns whether, for asynchronous calls, the caller
    is prepared to wait for interactive authorization.

    The default is \c false.

    \since 6.7
    \sa setInteractiveAuthorizationAllowed(),
        QDBusMessage::setInteractiveAuthorizationAllowed()
*/
bool QDBusAbstractInterface::isInteractiveAuthorizationAllowed() const
{
    return d_func()->interactiveAuthorizationAllowed;
}

/*!
    Places a call to the remote method specified by \a method on this interface, using \a args as
    arguments. This function returns the message that was received as a reply, which can be a normal
    QDBusMessage::ReplyMessage (indicating success) or QDBusMessage::ErrorMessage (if the call
    failed). The \a mode parameter specifies how this call should be placed.

    If the call succeeds, lastError() will be cleared; otherwise, it will contain the error this
    call produced.

    Normally, you should place calls using call().

    \warning If you use \c UseEventLoop, your code must be prepared to deal with any reentrancy:
             other method calls and signals may be delivered before this function returns, as well
             as other Qt queued signals and events.

    \threadsafe
*/
QDBusMessage QDBusAbstractInterface::callWithArgumentList(QDBus::CallMode mode,
                                                          const QString& method,
                                                          const QList<QVariant>& args)
{
    Q_D(QDBusAbstractInterface);

    if (!d->isValid || !d->canMakeCalls())
        return QDBusMessage::createError(d->lastError);

    QString m = method;
    // split out the signature from the method
    int pos = method.indexOf(u'.');
    if (pos != -1)
        m.truncate(pos);

    if (mode == QDBus::AutoDetect) {
        // determine if this a sync or async call
        mode = QDBus::Block;
        const QMetaObject *mo = metaObject();
        QByteArray match = m.toLatin1();

        for (int i = staticMetaObject.methodCount(); i < mo->methodCount(); ++i) {
            QMetaMethod mm = mo->method(i);
            if (mm.name() == match) {
                // found a method with the same name as what we're looking for
                // hopefully, nobody is overloading asynchronous and synchronous methods with
                // the same name

                QList<QByteArray> tags = QByteArray(mm.tag()).split(' ');
                if (tags.contains("Q_NOREPLY"))
                    mode = QDBus::NoBlock;

                break;
            }
        }
    }

//    qDebug() << "QDBusAbstractInterface" << "Service" << service() << "Path:" << path();
    QDBusMessage msg = QDBusMessage::createMethodCall(service(), path(), interface(), m);
    QDBusMessagePrivate::setParametersValidated(msg, true);
    msg.setArguments(args);

    QDBusMessage reply = d->connection.call(msg, mode, d->timeout);
    if (thread() == QThread::currentThread())
        d->lastError = QDBusError(reply);       // will clear if reply isn't an error

    // ensure that there is at least one element
    if (reply.arguments().isEmpty())
        reply << QVariant();

    return reply;
}

/*!
    \since 4.5
    Places a call to the remote method specified by \a method on this
    interface, using \a args as arguments. This function returns a
    QDBusPendingCall object that can be used to track the status of the
    reply and access its contents once it has arrived.

    Normally, you should place calls using asyncCall().

    \note Method calls to objects registered by the application itself are never
    asynchronous due to implementation limitations.

    \threadsafe
*/
QDBusPendingCall QDBusAbstractInterface::asyncCallWithArgumentList(const QString& method,
                                                                   const QList<QVariant>& args)
{
    Q_D(QDBusAbstractInterface);

    if (!d->isValid || !d->canMakeCalls())
        return QDBusPendingCall::fromError(d->lastError);

    QDBusMessage msg = QDBusMessage::createMethodCall(service(), path(), interface(), method);
    QDBusMessagePrivate::setParametersValidated(msg, true);
    msg.setArguments(args);
    if (d->interactiveAuthorizationAllowed)
        msg.setInteractiveAuthorizationAllowed(true);
    return d->connection.asyncCall(msg, d->timeout);
}

/*!
    Places a call to the remote method specified by \a method
    on this interface, using \a args as arguments. This function
    returns immediately after queueing the call. The reply from
    the remote function is delivered to the \a returnMethod on
    object \a receiver. If an error occurs, the \a errorMethod
    on object \a receiver is called instead.

    This function returns \c true if the queueing succeeds. It does
    not indicate that the executed call succeeded. If it fails,
    the \a errorMethod is called. If the queueing failed, this
    function returns \c false and no slot will be called.

    The \a returnMethod must have as its parameters the types returned
    by the function call. Optionally, it may have a QDBusMessage
    parameter as its last or only parameter.  The \a errorMethod must
    have a QDBusError as its only parameter.

    \note Method calls to objects registered by the application itself are never
    asynchronous due to implementation limitations.

    \since 4.3
    \sa QDBusError, QDBusMessage
 */
bool QDBusAbstractInterface::callWithCallback(const QString &method,
                                              const QList<QVariant> &args,
                                              QObject *receiver,
                                              const char *returnMethod,
                                              const char *errorMethod)
{
    Q_D(QDBusAbstractInterface);

    if (!d->isValid || !d->canMakeCalls())
        return false;

    QDBusMessage msg = QDBusMessage::createMethodCall(service(),
                                                      path(),
                                                      interface(),
                                                      method);
    QDBusMessagePrivate::setParametersValidated(msg, true);
    msg.setArguments(args);

    d->lastError = QDBusError();
    return d->connection.callWithCallback(msg,
                                          receiver,
                                          returnMethod,
                                          errorMethod,
                                          d->timeout);
}

/*!
    \overload

    This function is deprecated. Please use the overloaded version.

    Places a call to the remote method specified by \a method
    on this interface, using \a args as arguments. This function
    returns immediately after queueing the call. The reply from
    the remote function or any errors emitted by it are delivered
    to the \a slot slot on object \a receiver.

    This function returns \c true if the queueing succeeded: it does
    not indicate that the call succeeded. If it failed, the slot
    will be called with an error message. lastError() will not be
    set under those circumstances.

    \sa QDBusError, QDBusMessage
*/
bool QDBusAbstractInterface::callWithCallback(const QString &method,
                                              const QList<QVariant> &args,
                                              QObject *receiver,
                                              const char *slot)
{
    return callWithCallback(method, args, receiver, slot, nullptr);
}

/*!
    \internal
    Catch signal connections.
*/
void QDBusAbstractInterface::connectNotify(const QMetaMethod &signal)
{
    // someone connecting to one of our signals
    Q_D(QDBusAbstractInterface);
    if (!d->isValid)
        return;

    // we end up recursing here, so optimize away
    static const QMetaMethod destroyedSignal = QMetaMethod::fromSignal(&QDBusAbstractInterface::destroyed);
    if (signal == destroyedSignal)
        return;

    QDBusConnectionPrivate *conn = d->connectionPrivate();
    if (conn) {
        conn->connectRelay(d->service, d->path, d->interface,
                           this, signal);
    }
}

/*!
    \internal
    Catch signal disconnections.
*/
void QDBusAbstractInterface::disconnectNotify(const QMetaMethod &signal)
{
    // someone disconnecting from one of our signals
    Q_D(QDBusAbstractInterface);
    if (!d->isValid)
        return;

    // disconnection is just resource freeing, so it can be delayed;
    // let's do that later, after all the QObject mutexes have been unlocked.
    QCoreApplication::postEvent(this, new DisconnectRelayEvent(this, signal));
}

/*!
    \internal
    Continues the disconnect notification from above.
*/
void QDBusAbstractInterfacePrivate::finishDisconnectNotify(QDBusAbstractInterface *ptr, int signalId)
{
    QDBusAbstractInterfacePrivate *d = ptr->d_func();
    QDBusConnectionPrivate *conn = d->connectionPrivate();
    if (!conn)
        return;

    const QMetaObject *mo = ptr->metaObject();
    QMetaMethod signal = signalId >= 0 ? mo->method(signalId) : QMetaMethod();
    if (signal.isValid()) {
        if (!ptr->isSignalConnected(signal))
            return conn->disconnectRelay(d->service, d->path, d->interface,
                                         ptr, signal);
    } else {
        // wildcard disconnecting, we need to figure out which of our signals are
        // no longer connected to anything
        int midx = QObject::staticMetaObject.methodCount();
        const int end = mo->methodCount();
        for ( ; midx < end; ++midx) {
            QMetaMethod mm = mo->method(midx);
            if (mm.methodType() == QMetaMethod::Signal && !ptr->isSignalConnected(mm))
                conn->disconnectRelay(d->service, d->path, d->interface, ptr, mm);
        }
    }
}

/*!
    \internal
    Get the value of the property \a propname.
*/
QVariant QDBusAbstractInterface::internalPropGet(const char *propname) const
{
    // assume this property exists and is readable
    // we're only called from generated code anyways

    return property(propname);
}

/*!
    \internal
    Set the value of the property \a propname to \a value.
*/
void QDBusAbstractInterface::internalPropSet(const char *propname, const QVariant &value)
{
    setProperty(propname, value);
}

/*!
    \fn QDBusAbstractInterface::call(const QString &message)
    \internal
*/

/*!
    \fn template <typename...Args> QDBusMessage QDBusAbstractInterface::call(const QString &method, Args&&...args)

    Calls the method \a method on this interface and passes \a args to the method.
    All \a args must be convertible to QVariant.

    The parameters to \c call are passed on to the remote function via D-Bus as input
    arguments. Output arguments are returned in the QDBusMessage reply. If the reply is an error
    reply, lastError() will also be set to the contents of the error message.

    It can be used the following way:

    \snippet code/src_qdbus_qdbusabstractinterface.cpp 0

    This example illustrates function calling with 0, 1 and 2 parameters and illustrates different
    parameter types passed in each (the first call to \c "ProcessWorkUnicode" will contain one
    Unicode string, the second call to \c "ProcessWork" will contain one string and one byte array).

    \note Before Qt 5.14, this function accepted a maximum of just eight (8) arguments.

    \sa callWithArgumentList()
*/

/*!
    \fn QDBusAbstractInterface::call(QDBus::CallMode mode, const QString &message)
    \internal
*/

/*!
    \fn template <typename...Args> QDBusMessage QDBusAbstractInterface::call(QDBus::CallMode mode, const QString &method, Args&&...args)

    \overload

    Calls the method \a method on this interface and passes \a args to the method.
    All \a args must be convertible to QVariant.

    If \a mode is \c NoWaitForReply, then this function will return immediately after
    placing the call, without waiting for a reply from the remote
    method. Otherwise, \a mode indicates whether this function should
    activate the Qt Event Loop while waiting for the reply to arrive.

    If this function reenters the Qt event loop in order to wait for the
    reply, it will exclude user input. During the wait, it may deliver
    signals and other method calls to your application. Therefore, it
    must be prepared to handle a reentrancy whenever a call is placed
    with call().

    \note Before Qt 5.14, this function accepted a maximum of just eight (8) arguments.

    \sa callWithArgumentList()
*/

/*!
    \fn QDBusAbstractInterface::asyncCall(const QString &message)
    \internal
*/

/*!
    \fn template <typename...Args> QDBusPendingCall QDBusAbstractInterface::asyncCall(const QString &method, Args&&...args)

    Calls the method \a method on this interface and passes \a args to the method.
    All \a args must be convertible to QVariant.

    The parameters to \c call are passed on to the remote function via D-Bus as input
    arguments. The returned QDBusPendingCall object can be used to find out information about
    the reply.

    It can be used the following way:

    \snippet code/src_qdbus_qdbusabstractinterface.cpp 1

    This example illustrates function calling with 0, 1 and 2 parameters and illustrates different
    parameter types passed in each (the first call to \c "ProcessWorkUnicode" will contain one
    Unicode string, the second call to \c "ProcessWork" will contain one string and one byte array).

    \note Before Qt 5.14, this function accepted a maximum of just eight (8) arguments.

    \note Method calls to local \c{QDBusServer}'s are never asynchronous
    due to implementation limitations.

    \sa asyncCallWithArgumentList()
*/


/*!
    \internal
*/
QDBusMessage QDBusAbstractInterface::internalConstCall(QDBus::CallMode mode,
                                                       const QString &method,
                                                       const QList<QVariant> &args) const
{
    // ### move the code here, and make the other functions call this
    return const_cast<QDBusAbstractInterface*>(this)->callWithArgumentList(mode, method, args);
}

QDBusMessage QDBusAbstractInterface::doCall(QDBus::CallMode mode, const QString &method, const QVariant *args, size_t numArgs)
{
    QList<QVariant> list;
    list.reserve(int(numArgs));
    for (size_t i = 0; i < numArgs; ++i)
        list.append(args[i]);
    return callWithArgumentList(mode, method, list);
}

QDBusPendingCall QDBusAbstractInterface::doAsyncCall(const QString &method, const QVariant *args, size_t numArgs)
{
    QList<QVariant> list;
    list.reserve(int(numArgs));
    for (size_t i = 0; i < numArgs; ++i)
        list.append(args[i]);
    return asyncCallWithArgumentList(method, list);
}

QT_END_NAMESPACE

#endif // QT_NO_DBUS

#include "moc_qdbusabstractinterface.cpp"
