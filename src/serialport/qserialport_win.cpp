/****************************************************************************
**
** Copyright (C) 2012 Denis Shienkov <denis.shienkov@gmail.com>
** Copyright (C) 2012 Laszlo Papp <lpapp@kde.org>
** Copyright (C) 2012 Andre Hartmann <aha_1980@gmx.de>
** Contact: https://www.qt.io/licensing/
**
** This file is part of the QtSerialPort module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:COMM$
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** $QT_END_LICENSE$
**
**
**
**
**
**
**
**
**
**
**
**
**
**
**
**
**
**
**
****************************************************************************/

#include "qserialport_p.h"
#include "qtntdll_p.h"

#include <QtCore/qcoreevent.h>
#include <QtCore/qelapsedtimer.h>
#include <QtCore/qmutex.h>
#include <QtCore/qtimer.h>
#include <QtCore/qvector.h>
#include <algorithm>

QT_BEGIN_NAMESPACE

static inline void qt_set_common_props(DCB *dcb)
{
    dcb->fBinary = TRUE;
    dcb->fAbortOnError = FALSE;
    dcb->fNull = FALSE;
    dcb->fErrorChar = FALSE;

    if (dcb->fDtrControl == DTR_CONTROL_HANDSHAKE)
        dcb->fDtrControl = DTR_CONTROL_DISABLE;

    if (dcb->fRtsControl != RTS_CONTROL_HANDSHAKE)
        dcb->fRtsControl = RTS_CONTROL_DISABLE;
}

static inline void qt_set_baudrate(DCB *dcb, qint32 baudrate)
{
    dcb->BaudRate = baudrate;
}

static inline void qt_set_databits(DCB *dcb, QSerialPort::DataBits databits)
{
    dcb->ByteSize = databits;
}

static inline void qt_set_parity(DCB *dcb, QSerialPort::Parity parity)
{
    dcb->fParity = TRUE;
    switch (parity) {
    case QSerialPort::NoParity:
        dcb->Parity = NOPARITY;
        dcb->fParity = FALSE;
        break;
    case QSerialPort::OddParity:
        dcb->Parity = ODDPARITY;
        break;
    case QSerialPort::EvenParity:
        dcb->Parity = EVENPARITY;
        break;
    case QSerialPort::MarkParity:
        dcb->Parity = MARKPARITY;
        break;
    case QSerialPort::SpaceParity:
        dcb->Parity = SPACEPARITY;
        break;
    default:
        dcb->Parity = NOPARITY;
        dcb->fParity = FALSE;
        break;
    }
}

static inline void qt_set_stopbits(DCB *dcb, QSerialPort::StopBits stopbits)
{
    switch (stopbits) {
    case QSerialPort::OneStop:
        dcb->StopBits = ONESTOPBIT;
        break;
    case QSerialPort::OneAndHalfStop:
        dcb->StopBits = ONE5STOPBITS;
        break;
    case QSerialPort::TwoStop:
        dcb->StopBits = TWOSTOPBITS;
        break;
    default:
        dcb->StopBits = ONESTOPBIT;
        break;
    }
}

static inline void qt_set_flowcontrol(DCB *dcb, QSerialPort::FlowControl flowcontrol)
{
    dcb->fInX = FALSE;
    dcb->fOutX = FALSE;
    dcb->fOutxCtsFlow = FALSE;
    if (dcb->fRtsControl == RTS_CONTROL_HANDSHAKE)
        dcb->fRtsControl = RTS_CONTROL_DISABLE;
    switch (flowcontrol) {
    case QSerialPort::NoFlowControl:
        break;
    case QSerialPort::SoftwareControl:
        dcb->fInX = TRUE;
        dcb->fOutX = TRUE;
        break;
    case QSerialPort::HardwareControl:
        dcb->fOutxCtsFlow = TRUE;
        dcb->fRtsControl = RTS_CONTROL_HANDSHAKE;
        break;
    default:
        break;
    }
}

// Translate NT-callbacks to Win32 callbacks.
static VOID WINAPI qt_apc_routine(
        PVOID context,
        PIO_STATUS_BLOCK ioStatusBlock,
        DWORD reserved)
{
    Q_UNUSED(reserved);

    const DWORD errorCode = ::RtlNtStatusToDosError(ioStatusBlock->Status);
    const DWORD bytesTransfered = NT_SUCCESS(ioStatusBlock->Status)
            ? DWORD(ioStatusBlock->Information) : 0;
    const LPOVERLAPPED overlapped = CONTAINING_RECORD(ioStatusBlock,
                                                      OVERLAPPED, Internal);

    (reinterpret_cast<LPOVERLAPPED_COMPLETION_ROUTINE>(context))
            (errorCode, bytesTransfered, overlapped);
}

// Alertable analog of DeviceIoControl function.
static BOOL qt_device_io_control_ex(
        HANDLE deviceHandle,
        DWORD ioControlCode,
        LPVOID inputBuffer,
        DWORD inputBufferSize,
        LPVOID outputBuffer,
        DWORD outputBufferSize,
        LPOVERLAPPED overlapped,
        LPOVERLAPPED_COMPLETION_ROUTINE completionRoutine)
{
    const auto ioStatusBlock = reinterpret_cast<PIO_STATUS_BLOCK>(
                &overlapped->Internal);
    ioStatusBlock->Status = STATUS_PENDING;

    const NTSTATUS status = ::NtDeviceIoControlFile(
                deviceHandle,
                nullptr,
                qt_apc_routine,
                reinterpret_cast<PVOID>(completionRoutine),
                ioStatusBlock,
                ioControlCode,
                inputBuffer,
                inputBufferSize,
                outputBuffer,
                outputBufferSize);

    if (!NT_SUCCESS(status)) {
        ::SetLastError(::RtlNtStatusToDosError(status));
        return false;
    }

    return true;
}

// Alertable analog of WaitCommEvent function.
static BOOL qt_wait_comm_event_ex(
        HANDLE deviceHandle,
        LPDWORD eventsMask,
        LPOVERLAPPED overlapped,
        LPOVERLAPPED_COMPLETION_ROUTINE completionRoutine)
{
    return qt_device_io_control_ex(
                deviceHandle,
                IOCTL_SERIAL_WAIT_ON_MASK,
                nullptr,
                0,
                eventsMask,
                sizeof(DWORD),
                overlapped,
                completionRoutine);
}

struct RuntimeHelper
{
    QLibrary ntLibrary;
    QBasicMutex mutex;
};

Q_GLOBAL_STATIC(RuntimeHelper, helper)

class Overlapped final : public OVERLAPPED
{
    Q_DISABLE_COPY(Overlapped)
public:
    explicit Overlapped(QSerialPortPrivate *d);
    void clear();

    QSerialPortPrivate *dptr = nullptr;
};

Overlapped::Overlapped(QSerialPortPrivate *d)
    : dptr(d)
{
}

void Overlapped::clear()
{
    ::ZeroMemory(this, sizeof(OVERLAPPED));
}

bool QSerialPortPrivate::open(QIODevice::OpenMode mode)
{
    {
        QMutexLocker locker(&helper()->mutex);
        static bool symbolsResolved = resolveNtdllSymbols(&helper()->ntLibrary);
        if (!symbolsResolved) {
            setError(QSerialPortErrorInfo(QSerialPort::OpenError,
                                          helper()->ntLibrary.errorString()));
            return false;
        }
    }

    DWORD desiredAccess = 0;

    if (mode & QIODevice::ReadOnly)
        desiredAccess |= GENERIC_READ;
    if (mode & QIODevice::WriteOnly)
        desiredAccess |= GENERIC_WRITE;

    handle = ::CreateFile(reinterpret_cast<const wchar_t*>(systemLocation.utf16()),
                              desiredAccess, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);

    if (handle == INVALID_HANDLE_VALUE) {
        setError(getSystemError());
        return false;
    }

    if (initialize(mode))
        return true;

    ::CloseHandle(handle);
    return false;
}

void QSerialPortPrivate::close()
{
    delete startAsyncWriteTimer;
    startAsyncWriteTimer = nullptr;

    if (communicationStarted) {
        communicationCompletionOverlapped->dptr = nullptr;
        ::CancelIoEx(handle, communicationCompletionOverlapped);
        // The object will be deleted in the I/O callback.
        communicationCompletionOverlapped = nullptr;
        communicationStarted = false;
    } else {
        delete communicationCompletionOverlapped;
        communicationCompletionOverlapped = nullptr;
    }

    if (readStarted) {
        readCompletionOverlapped->dptr = nullptr;
        ::CancelIoEx(handle, readCompletionOverlapped);
        // The object will be deleted in the I/O callback.
        readCompletionOverlapped = nullptr;
        readStarted = false;
    } else {
        delete readCompletionOverlapped;
        readCompletionOverlapped = nullptr;
    };

    if (writeStarted) {
        writeCompletionOverlapped->dptr = nullptr;
        ::CancelIoEx(handle, writeCompletionOverlapped);
        // The object will be deleted in the I/O callback.
        writeCompletionOverlapped = nullptr;
        writeStarted = false;
    } else {
        delete writeCompletionOverlapped;
        writeCompletionOverlapped = nullptr;
    }

    readBytesTransferred = 0;
    writeBytesTransferred = 0;
    writeBuffer.clear();

    if (settingsRestoredOnClose) {
        ::SetCommState(handle, &restoredDcb);
        ::SetCommTimeouts(handle, &restoredCommTimeouts);
    }

    ::CloseHandle(handle);
    handle = INVALID_HANDLE_VALUE;
}

QSerialPort::PinoutSignals QSerialPortPrivate::pinoutSignals()
{
    DWORD modemStat = 0;

    if (!::GetCommModemStatus(handle, &modemStat)) {
        setError(getSystemError());
        return QSerialPort::NoSignal;
    }

    QSerialPort::PinoutSignals ret = QSerialPort::NoSignal;

    if (modemStat & MS_CTS_ON)
        ret |= QSerialPort::ClearToSendSignal;
    if (modemStat & MS_DSR_ON)
        ret |= QSerialPort::DataSetReadySignal;
    if (modemStat & MS_RING_ON)
        ret |= QSerialPort::RingIndicatorSignal;
    if (modemStat & MS_RLSD_ON)
        ret |= QSerialPort::DataCarrierDetectSignal;

    DWORD bytesReturned = 0;
    if (!::DeviceIoControl(handle, IOCTL_SERIAL_GET_DTRRTS, nullptr, 0,
                          &modemStat, sizeof(modemStat),
                          &bytesReturned, nullptr)) {
        setError(getSystemError());
        return ret;
    }

    if (modemStat & SERIAL_DTR_STATE)
        ret |= QSerialPort::DataTerminalReadySignal;
    if (modemStat & SERIAL_RTS_STATE)
        ret |= QSerialPort::RequestToSendSignal;

    return ret;
}

bool QSerialPortPrivate::setDataTerminalReady(bool set)
{
    if (!::EscapeCommFunction(handle, set ? SETDTR : CLRDTR)) {
        setError(getSystemError());
        return false;
    }

    DCB dcb;
    if (!getDcb(&dcb))
        return false;

    dcb.fDtrControl = set ? DTR_CONTROL_ENABLE : DTR_CONTROL_DISABLE;
    return setDcb(&dcb);
}

bool QSerialPortPrivate::setRequestToSend(bool set)
{
    if (!::EscapeCommFunction(handle, set ? SETRTS : CLRRTS)) {
        setError(getSystemError());
        return false;
    }

    DCB dcb;
    if (!getDcb(&dcb))
        return false;

    dcb.fRtsControl = set ? RTS_CONTROL_ENABLE : RTS_CONTROL_DISABLE;
    return setDcb(&dcb);
}

bool QSerialPortPrivate::flush()
{
    return _q_startAsyncWrite();
}

bool QSerialPortPrivate::clear(QSerialPort::Directions directions)
{
    DWORD flags = 0;
    if (directions & QSerialPort::Input)
        flags |= PURGE_RXABORT | PURGE_RXCLEAR;
    if (directions & QSerialPort::Output)
        flags |= PURGE_TXABORT | PURGE_TXCLEAR;
    if (!::PurgeComm(handle, flags)) {
        setError(getSystemError());
        return false;
    }

    // We need start async read because a reading can be stalled. Since the
    // PurgeComm can abort of current reading sequence, or a port is in hardware
    // flow control mode, or a port has a limited read buffer size.
    if (directions & QSerialPort::Input)
        startAsyncCommunication();

    return true;
}

bool QSerialPortPrivate::sendBreak(int duration)
{
    if (!setBreakEnabled(true))
        return false;

    ::Sleep(duration);

    if (!setBreakEnabled(false))
        return false;

    return true;
}

bool QSerialPortPrivate::setBreakEnabled(bool set)
{
    if (set ? !::SetCommBreak(handle) : !::ClearCommBreak(handle)) {
        setError(getSystemError());
        return false;
    }

    return true;
}

bool QSerialPortPrivate::waitForReadyRead(int msecs)
{
    if (!writeStarted && !_q_startAsyncWrite())
        return false;

    QDeadlineTimer deadline(msecs);

    do {
        if (readBytesTransferred <= 0) {
            const qint64 remaining = deadline.remainingTime();
            const DWORD result = ::SleepEx(
                        remaining == -1 ? INFINITE : DWORD(remaining),
                        TRUE);
            if (result != WAIT_IO_COMPLETION)
                continue;
        }

        if (readBytesTransferred > 0) {
            readBytesTransferred = 0;
            return true;
        }
    } while (!deadline.hasExpired());

    setError(getSystemError(WAIT_TIMEOUT));
    return false;
}

bool QSerialPortPrivate::waitForBytesWritten(int msecs)
{
    if (writeBuffer.isEmpty() && writeChunkBuffer.isEmpty())
        return false;

    if (!writeStarted && !_q_startAsyncWrite())
        return false;

    QDeadlineTimer deadline(msecs);

    do {
        if (writeBytesTransferred <= 0) {
            const qint64 remaining = deadline.remainingTime();
            const DWORD result = ::SleepEx(
                        remaining == -1 ? INFINITE : DWORD(remaining),
                        TRUE);
            if (result != WAIT_IO_COMPLETION)
                continue;
        }

        if (writeBytesTransferred > 0) {
            writeBytesTransferred = 0;
            return true;
        }
    } while (!deadline.hasExpired());

    setError(getSystemError(WAIT_TIMEOUT));
    return false;
}

bool QSerialPortPrivate::setBaudRate()
{
    return setBaudRate(inputBaudRate, QSerialPort::AllDirections);
}

bool QSerialPortPrivate::setBaudRate(qint32 baudRate, QSerialPort::Directions directions)
{
    if (directions != QSerialPort::AllDirections) {
        setError(QSerialPortErrorInfo(QSerialPort::UnsupportedOperationError, QSerialPort::tr("Custom baud rate direction is unsupported")));
        return false;
    }

    DCB dcb;
    if (!getDcb(&dcb))
        return false;

    qt_set_baudrate(&dcb, baudRate);

    return setDcb(&dcb);
}

bool QSerialPortPrivate::setDataBits(QSerialPort::DataBits dataBits)
{
    DCB dcb;
    if (!getDcb(&dcb))
        return false;

    qt_set_databits(&dcb, dataBits);

    return setDcb(&dcb);
}

bool QSerialPortPrivate::setParity(QSerialPort::Parity parity)
{
    DCB dcb;
    if (!getDcb(&dcb))
        return false;

    qt_set_parity(&dcb, parity);

    return setDcb(&dcb);
}

bool QSerialPortPrivate::setStopBits(QSerialPort::StopBits stopBits)
{
    DCB dcb;
    if (!getDcb(&dcb))
        return false;

    qt_set_stopbits(&dcb, stopBits);

    return setDcb(&dcb);
}

bool QSerialPortPrivate::setFlowControl(QSerialPort::FlowControl flowControl)
{
    DCB dcb;
    if (!getDcb(&dcb))
        return false;

    qt_set_flowcontrol(&dcb, flowControl);

    return setDcb(&dcb);
}

bool QSerialPortPrivate::completeAsyncCommunication(qint64 bytesTransferred)
{
    communicationStarted = false;

    if (bytesTransferred == qint64(-1))
        return false;

    return startAsyncRead();
}

bool QSerialPortPrivate::completeAsyncRead(qint64 bytesTransferred)
{
    // Store the number of transferred bytes which are
    // required only in waitForReadyRead() method.
    readBytesTransferred = bytesTransferred;

    if (bytesTransferred == qint64(-1)) {
        readStarted = false;
        return false;
    }
    if (bytesTransferred > 0)
        buffer.append(readChunkBuffer.constData(), bytesTransferred);

    readStarted = false;

    bool result = true;
    if (bytesTransferred == QSERIALPORT_BUFFERSIZE
            || queuedBytesCount(QSerialPort::Input) > 0) {
        result = startAsyncRead();
    } else {
        result = startAsyncCommunication();
    }

    if (bytesTransferred > 0)
        emitReadyRead();

    return result;
}

bool QSerialPortPrivate::completeAsyncWrite(qint64 bytesTransferred)
{
    Q_Q(QSerialPort);

    // Store the number of transferred bytes which are
    // required only in waitForBytesWritten() method.
    writeBytesTransferred = bytesTransferred;

    if (writeStarted) {
        if (bytesTransferred == qint64(-1)) {
            writeChunkBuffer.clear();
            writeStarted = false;
            return false;
        }
        Q_ASSERT(bytesTransferred == writeChunkBuffer.size());
        writeChunkBuffer.clear();
        emit q->bytesWritten(bytesTransferred);
        writeStarted = false;
    }

    return _q_startAsyncWrite();
}

bool QSerialPortPrivate::startAsyncCommunication()
{
    if (communicationStarted)
        return true;

    if (!communicationCompletionOverlapped)
        communicationCompletionOverlapped = new Overlapped(this);

    communicationCompletionOverlapped->clear();
    communicationStarted = true;
    if (!::qt_wait_comm_event_ex(handle,
                                 &triggeredEventMask,
                                 communicationCompletionOverlapped,
                                 ioCompletionRoutine)) {
        communicationStarted = false;
        QSerialPortErrorInfo error = getSystemError();
        if (error.errorCode != QSerialPort::NoError) {
            if (error.errorCode == QSerialPort::PermissionError)
                error.errorCode = QSerialPort::ResourceError;
            setError(error);
            return false;
        }
    }
    return true;
}

bool QSerialPortPrivate::startAsyncRead()
{
    if (readStarted)
        return true;

    qint64 bytesToRead = QSERIALPORT_BUFFERSIZE;

    if (readBufferMaxSize && bytesToRead > (readBufferMaxSize - buffer.size())) {
        bytesToRead = readBufferMaxSize - buffer.size();
        if (bytesToRead <= 0) {
            // Buffer is full. User must read data from the buffer
            // before we can read more from the port.
            return false;
        }
    }

    Q_ASSERT(int(bytesToRead) <= readChunkBuffer.size());

    if (!readCompletionOverlapped)
        readCompletionOverlapped = new Overlapped(this);

    readCompletionOverlapped->clear();
    readStarted = true;
    if (!::ReadFileEx(handle,
                      readChunkBuffer.data(),
                      bytesToRead,
                      readCompletionOverlapped,
                      ioCompletionRoutine)) {
        readStarted = false;
        QSerialPortErrorInfo error = getSystemError();
        if (error.errorCode != QSerialPort::NoError) {
            if (error.errorCode == QSerialPort::PermissionError)
                error.errorCode = QSerialPort::ResourceError;
            if (error.errorCode != QSerialPort::ResourceError)
                error.errorCode = QSerialPort::ReadError;
            setError(error);
            return false;
        }
    }
    return true;
}

bool QSerialPortPrivate::_q_startAsyncWrite()
{
    if (writeBuffer.isEmpty() || writeStarted)
        return true;

    writeChunkBuffer = writeBuffer.read();

    if (!writeCompletionOverlapped)
        writeCompletionOverlapped = new Overlapped(this);

    writeCompletionOverlapped->clear();
    writeStarted = true;
    if (!::WriteFileEx(handle,
                       writeChunkBuffer.constData(),
                       writeChunkBuffer.size(),
                       writeCompletionOverlapped,
                       ioCompletionRoutine)) {
        writeStarted = false;
        QSerialPortErrorInfo error = getSystemError();
        if (error.errorCode != QSerialPort::NoError) {
            if (error.errorCode != QSerialPort::ResourceError)
                error.errorCode = QSerialPort::WriteError;
            setError(error);
            return false;
        }
    }
    return true;
}

void QSerialPortPrivate::handleNotification(DWORD bytesTransferred, DWORD errorCode,
                                            OVERLAPPED *overlapped)
{
    // This occurred e.g. after calling the CloseHandle() function,
    // just skip handling at all.
    if (handle == INVALID_HANDLE_VALUE)
        return;

    const QSerialPortErrorInfo error = getSystemError(errorCode);
    if (error.errorCode != QSerialPort::NoError) {
        setError(error);
        return;
    }

    if (overlapped == communicationCompletionOverlapped)
        completeAsyncCommunication(bytesTransferred);
    else if (overlapped == readCompletionOverlapped)
        completeAsyncRead(bytesTransferred);
    else if (overlapped == writeCompletionOverlapped)
        completeAsyncWrite(bytesTransferred);
    else
        Q_ASSERT(!"Unknown OVERLAPPED activated");
}

void QSerialPortPrivate::emitReadyRead()
{
    Q_Q(QSerialPort);

    emit q->readyRead();
}

qint64 QSerialPortPrivate::writeData(const char *data, qint64 maxSize)
{
    Q_Q(QSerialPort);

    writeBuffer.append(data, maxSize);

    if (!writeBuffer.isEmpty() && !writeStarted) {
        if (!startAsyncWriteTimer) {
            startAsyncWriteTimer = new QTimer(q);
            QObjectPrivate::connect(startAsyncWriteTimer, &QTimer::timeout, this, &QSerialPortPrivate::_q_startAsyncWrite);
            startAsyncWriteTimer->setSingleShot(true);
        }
        if (!startAsyncWriteTimer->isActive())
            startAsyncWriteTimer->start();
    }
    return maxSize;
}

qint64 QSerialPortPrivate::queuedBytesCount(QSerialPort::Direction direction) const
{
    COMSTAT comstat;
    if (::ClearCommError(handle, nullptr, &comstat) == 0)
        return -1;
    return (direction == QSerialPort::Input)
            ? comstat.cbInQue
            : ((direction == QSerialPort::Output) ? comstat.cbOutQue : -1);
}

inline bool QSerialPortPrivate::initialize(QIODevice::OpenMode mode)
{
    DCB dcb;
    if (!getDcb(&dcb))
        return false;

    restoredDcb = dcb;

    qt_set_common_props(&dcb);
    qt_set_baudrate(&dcb, inputBaudRate);
    qt_set_databits(&dcb, dataBits);
    qt_set_parity(&dcb, parity);
    qt_set_stopbits(&dcb, stopBits);
    qt_set_flowcontrol(&dcb, flowControl);

    if (!setDcb(&dcb))
        return false;

    if (!::GetCommTimeouts(handle, &restoredCommTimeouts)) {
        setError(getSystemError());
        return false;
    }

    ::ZeroMemory(&currentCommTimeouts, sizeof(currentCommTimeouts));
    currentCommTimeouts.ReadIntervalTimeout = MAXDWORD;

    if (!::SetCommTimeouts(handle, &currentCommTimeouts)) {
        setError(getSystemError());
        return false;
    }

    const DWORD eventMask = (mode & QIODevice::ReadOnly) ? EV_RXCHAR : 0;
    if (!::SetCommMask(handle, eventMask)) {
        setError(getSystemError());
        return false;
    }

    if ((eventMask & EV_RXCHAR) && !startAsyncCommunication())
        return false;

    return true;
}

bool QSerialPortPrivate::setDcb(DCB *dcb)
{
    if (!::SetCommState(handle, dcb)) {
        setError(getSystemError());
        return false;
    }
    return true;
}

bool QSerialPortPrivate::getDcb(DCB *dcb)
{
    ::ZeroMemory(dcb, sizeof(DCB));
    dcb->DCBlength = sizeof(DCB);

    if (!::GetCommState(handle, dcb)) {
        setError(getSystemError());
        return false;
    }
    return true;
}

QSerialPortErrorInfo QSerialPortPrivate::getSystemError(int systemErrorCode) const
{
    if (systemErrorCode == -1)
        systemErrorCode = ::GetLastError();

    QSerialPortErrorInfo error;
    error.errorString = qt_error_string(systemErrorCode);

    switch (systemErrorCode) {
    case ERROR_SUCCESS:
        error.errorCode = QSerialPort::NoError;
        break;
    case ERROR_IO_PENDING:
        error.errorCode = QSerialPort::NoError;
        break;
    case ERROR_MORE_DATA:
        error.errorCode = QSerialPort::NoError;
        break;
    case ERROR_FILE_NOT_FOUND:
        error.errorCode = QSerialPort::DeviceNotFoundError;
        break;
    case ERROR_PATH_NOT_FOUND:
        error.errorCode = QSerialPort::DeviceNotFoundError;
        break;
    case ERROR_INVALID_NAME:
        error.errorCode = QSerialPort::DeviceNotFoundError;
        break;
    case ERROR_ACCESS_DENIED:
        error.errorCode = QSerialPort::PermissionError;
        break;
    case ERROR_INVALID_HANDLE:
        error.errorCode = QSerialPort::ResourceError;
        break;
    case ERROR_INVALID_PARAMETER:
        error.errorCode = QSerialPort::UnsupportedOperationError;
        break;
    case ERROR_BAD_COMMAND:
        error.errorCode = QSerialPort::ResourceError;
        break;
    case ERROR_DEVICE_REMOVED:
        error.errorCode = QSerialPort::ResourceError;
        break;
    case ERROR_OPERATION_ABORTED:
        error.errorCode = QSerialPort::ResourceError;
        break;
    case WAIT_TIMEOUT:
        error.errorCode = QSerialPort::TimeoutError;
        break;
    default:
        error.errorCode = QSerialPort::UnknownError;
        break;
    }
    return error;
}

// This table contains standard values of baud rates that
// are defined in file winbase.h
QList<qint32> QSerialPortPrivate::standardBaudRates()
{
    static const QList<qint32> baudRates = {
        CBR_110,   CBR_300,   CBR_600,    CBR_1200,   CBR_2400,
        CBR_4800,  CBR_9600,  CBR_14400,  CBR_19200,  CBR_38400,
        CBR_56000, CBR_57600, CBR_115200, CBR_128000, CBR_256000
    };

    return baudRates;
}

QSerialPort::Handle QSerialPort::handle() const
{
    Q_D(const QSerialPort);
    return d->handle;
}

void QSerialPortPrivate::ioCompletionRoutine(
        DWORD errorCode, DWORD bytesTransfered,
        OVERLAPPED *overlappedBase)
{
    const auto overlapped = static_cast<Overlapped *>(overlappedBase);
    if (overlapped->dptr) {
        overlapped->dptr->handleNotification(bytesTransfered, errorCode,
                                             overlappedBase);
    } else {
        delete overlapped;
    }
}

QT_END_NAMESPACE
