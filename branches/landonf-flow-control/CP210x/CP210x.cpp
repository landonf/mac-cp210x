/*
 * Author: Landon Fuller <landonf@plausible.coop>
 *
 * Information on interfacing with the CP210x chipset was derived
 * from from the FreeBSD uslcom(4) and the publicly available
 * AN571 data sheet:
 * http://www.silabs.com/Support%20Documents/TechnicalDocs/AN571.pdf
 *
 * Copyright (c) 2012 Plausible Labs Cooperative, Inc.
 * All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <IOKit/IOLib.h>

#include "CP210x.h"
#include "CP210xConfig.h"
#include "RingBuffer.h"

#include "logging.h"

/* Default ring buffer size */
#define BUFFER_SIZE (PAGE_SIZE * 3)

/* Valid flow control arguments */
#define PL_RS232_A_FLOWCONTROL_MASK (PD_RS232_A_TXO | PD_RS232_A_XANY | PD_RS232_A_RXO | PD_RS232_A_RFR | PD_RS232_A_CTS | PD_RS232_A_DTR)

// Define the superclass
#define super IOSerialDriverSync

OSDefineMetaClassAndStructors(coop_plausible_driver_CP210x, super);

/** Mask on all possible state values */
#define STATE_ALL (PD_RS232_S_MASK | PD_S_MASK)

/** Mask on all state values that may be exposed externally */
#define STATE_EXTERNAL (PD_S_MASK | (PD_RS232_S_MASK & ~PD_RS232_S_LOOP))

/* Default read timeout in seconds */
#define READ_TIMEOUT 10 * 1000

#pragma mark Lifecycle Management

// from IOService base class 
IOService *coop_plausible_driver_CP210x::probe (IOService *provider, SInt32 *score) {
    IOService *res = super::probe(provider, score);
    LOG_DEBUG("probe");
    return res;
}

// from IOService base class
bool coop_plausible_driver_CP210x::init (OSDictionary *dict) {
    LOG_DEBUG("Driver initializing");

    if (!super::init(dict)) {
        LOG_ERR("super::init() failed");
        return false;
    }

    _lock = IOLockAlloc();
    _stopping = false;

    return true;
}

// from IOService base class
bool coop_plausible_driver_CP210x::start (IOService *provider) {
    LOG_DEBUG("Driver starting");

    if (!super::start(provider)) {
        LOG_ERR("super::start() failed");
        return false;
    }

    /* Reset port defaults. These will be set by the BSD termios intialization code path
     * on first open. */
    _baudRate = 0;
    _characterLength = 0;
    _txParity = PD_RS232_PARITY_DEFAULT;
    _rxParity = PD_RS232_PARITY_DEFAULT;
    _twoStopBits = false;
    _xonChar = '\0';
    _xoffChar = '\0';

    /* Fetch our USB provider */
    _provider = OSDynamicCast(IOUSBInterface, provider);
    if (_provider == NULL) {
        LOG_ERR("Received invalid provider");
        return false;
    }
    _provider->retain();
    
    /* Open USB interface */
    _provider->open(this);

    /* Find control endpoint */
    _controlPipe = _provider->GetPipeObj(0);
    if (_controlPipe == NULL) {
        /* Should never happen */
        LOG_ERR("Could not find control pipe");
        return false;
    }
    _controlPipe->retain();

    /* Find input endpoint */
    IOUSBFindEndpointRequest inReq = {
        .type = kUSBBulk,
        .direction = kUSBIn,
        .maxPacketSize = 0,
        .interval = 0
    };

    _inputPipe = _provider->FindNextPipe(NULL, &inReq, true);
    if (_inputPipe == NULL) {
        LOG_ERR("Could not find input pipe");
        return false;
    }
    _inputMaxPacketSize = inReq.maxPacketSize;
    if (_inputMaxPacketSize == 0) {
        LOG_ERR("Could not determine maximum input packet size, selecting minimum size of 8 bytes.");
        _inputMaxPacketSize = 8;
    }
    
    _receiveHandler.action = receiveHandler;
    _receiveHandler.target = this;
    _receiveHandler.parameter = NULL;

    /* Find output endpoint */
    IOUSBFindEndpointRequest outReq = {
        .type = kUSBBulk,
        .direction = kUSBOut,
        .maxPacketSize = 0,
        .interval = 0
    };
    _outputPipe = _provider->FindNextPipe(NULL, &outReq, true);
    if (_outputPipe == NULL) {
        LOG_ERR("Could not find output pipe");
        return false;
    }
    _outputMaxPacketSize = outReq.maxPacketSize;
    if (_outputMaxPacketSize == 0) {
        LOG_ERR("Could not determine maximum output packet size, selecting minimum size of 8 bytes.");
        _outputMaxPacketSize = 8;
    }

    _transmitHandler.action = transmitHandler;
    _transmitHandler.target = this;
    _transmitHandler.parameter = NULL;

    /* Configure TX/RX buffers */
    _txBuffer = new coop_plausible_CP210x_RingBuffer();
    if (!_txBuffer->init(BUFFER_SIZE)) {
        LOG_ERR("Could not create TX buffer");
        return false;
    }
    
    _rxBuffer = new coop_plausible_CP210x_RingBuffer();
    if (!_rxBuffer->init(BUFFER_SIZE)) {
        LOG_ERR("Could not create RX buffer");
        return false;
    }
    
    
    /* Initialize default state. This requires that the TX/RX buffers
     * already be initialized. */
    _state = 0;
    this->updateRXQueueState(NULL);
    this->updateTXQueueState(NULL);

    /* Create our child serial stream */
    _serialDevice = new coop_plausible_CP210x_SerialDevice();
    if (!_serialDevice->init(this, _provider)) {
        LOG_ERR("Could not create serial stream");
        return false;
    }

    LOG_INFO("USB serial port initialized.");

#if DEBUG
    /* Run the debug build unit tests */
    LOG_DEBUG("Running tests");
    coop_plausible_CP210x_RingBuffer_tests();
    LOG_DEBUG("Tests complete");
#endif

    return true;
}


// from IOService base class
void coop_plausible_driver_CP210x::stop (IOService *provider) {
    LOG_DEBUG("stop");
    
    IOLockLock(_lock); {
        /* Set _stopping flag, waking up any blocked/waiting threads. */
        this->setStopping();

        /* Abort all transfers */
        _inputPipe->Abort();
        _outputPipe->Abort();
        _controlPipe->Abort();

        /* Close our provider */
        _provider->close(this);
    } IOLockUnlock(_lock);
    
    super::stop(provider);
}

// from IOService base class;
bool coop_plausible_driver_CP210x::willTerminate (IOService *provider, IOOptionBits options) {
    LOG_DEBUG("willTerminate()");
    return super::willTerminate(provider, options);
}

// from IOService base class;
bool coop_plausible_driver_CP210x::didTerminate (IOService *provider, IOOptionBits options, bool *defer) {
    LOG_DEBUG("didTerminate()");
    return super::didTerminate(provider, options, defer);
}

// from IOService base class
void coop_plausible_driver_CP210x::free (void) {
    LOG_DEBUG("free\n");

    if (_lock != NULL) {
        IOLockFree(_lock);
        _lock = NULL;
    }
    
    if (_provider != NULL) {
        _provider->release();
        _provider = NULL;
    }
    
    if (_controlPipe != NULL) {
        _controlPipe->release();
        _controlPipe = NULL;
    }
    
    if (_inputPipe != NULL) {
        _inputPipe->release();
        _inputPipe = NULL;
    }
    
    if (_outputPipe != NULL) {
        _outputPipe->release();
        _outputPipe = NULL;
    }
    
    if (_serialDevice != NULL) {
        _serialDevice->release();
        _serialDevice = NULL;
    }
    
    if (_txBuffer != NULL) {
        _txBuffer->release();
        _txBuffer = NULL;
    }
    
    if (_rxBuffer != NULL) {
        _rxBuffer->release();
        _rxBuffer = NULL;
    }

    super::free();
}


// from IOSerialDriverSync
IOReturn coop_plausible_driver_CP210x::acquirePort(bool sleep, void *refCon) {
    LOG_DEBUG("Acquire Port");

    IOLockLock(_lock); {
        /* Verify that the driver has not been stopped */
        if (_stopping) {
            LOG_DEBUG("acquirePort() - offline (stopping)");
            IOLockUnlock(_lock);
            return kIOReturnOffline;
        }

        /* Verify that the port has not already been acquired, and if so, optionally
         * wait for it to be released. */
        while (_state & PD_S_ACQUIRED) {
            /* If blocking operation has not been requested, return immediately. */
            if (sleep == false) {
                IOLockUnlock(_lock);
                return kIOReturnExclusiveAccess;
            }

            /* Otherwise, wait for our requisite state */
            UInt32 reqState = 0;
            IOReturn rtn = watchState(&reqState, PD_S_ACQUIRED, refCon, true);
            if (rtn != kIOReturnSuccess) {
                IOLockUnlock(_lock);
                return rtn;
            }
        }

        /* Set initial port state */
        setState(PD_S_ACQUIRED, STATE_ALL, refCon, true);

        /* Ensure that RX and TX queue states are up-to-date while we still hold the lock. */
        updateRXQueueState(refCon);
        updateTXQueueState(refCon);

        /* Start asynchronous reading */
        this->startReceive(refCon);
    }
    IOLockUnlock(_lock);

    return kIOReturnSuccess;
}

// from IOSerialDriverSync
IOReturn coop_plausible_driver_CP210x::releasePort(void *refCon) {
    LOG_DEBUG("Release Port");

    IOLockLock(_lock); {
        /* Verify that the driver has not been stopped */
        if (_stopping) {
            LOG_DEBUG("releasePort() - offline (stopping)");
            IOLockUnlock(_lock);
            return kIOReturnOffline;
        }

        /* Validate that the port is actually open */
        if ((_state & PD_S_ACQUIRED) == 0) {
            IOLockUnlock(_lock);
            return kIOReturnNotOpen;
        }

        /* Abort I/O */
        _outputPipe->Abort();
        _inputPipe->Abort();

        /* Clear all buffers */
        _txBuffer->flush();
        _rxBuffer->flush();

        /* Reset the state to closed */
        setState(0, STATE_ALL, refCon, true);

        /* Ensure that RX and TX queue states are correct while we still hold the lock. */
        updateRXQueueState(refCon);
        updateTXQueueState(refCon);
    }
    IOLockUnlock(_lock);

    return kIOReturnSuccess;
}

/**
 * Increment the I/O reference count to account for a not-yet-completed
 * asynchronous I/O request.
 *
 * @param haveLock If true, the method will assume that _lock is held. If false, the lock will be acquired
 * automatically.
 */
void coop_plausible_driver_CP210x::incrIOReqCount (bool haveLock) {
    if (!haveLock)
        IOLockLock(_lock);
    
    _ioReqCount++;

    if (!haveLock)
        IOLockUnlock(_lock);
}

/**
 * Decrement the I/O reference count to account for a now-completed
 * asynchronous I/O request.
 *
 * @param haveLock If true, the method will assume that _lock is held. If false, the lock will be acquired
 * automatically.
 */
void coop_plausible_driver_CP210x::decrIOReqCount (bool haveLock) {
    if (!haveLock)
        IOLockLock(_lock);
    
    _ioReqCount--;
    if (_ioReqCount == 0 && _stopping) {
        // TODO
    }

    if (!haveLock)
        IOLockUnlock(_lock);
}

#pragma mark State Management

/**
 * Mark the driver as stopped. This will wake up any background threads blocked on our internal
 * semaphore, at which point they will check the _stopping flag and terminate gracefully.
 *
 * @warning This method must be called with _lock held.
 * @warning This method must only be called once.
 */
void coop_plausible_driver_CP210x::setStopping (void) {
    LOG_DEBUG("setStopping()");

    /* Mark driver as stopped */
    assert(!_stopping);
    _stopping = true;

    /* Wake up all waiting threads. */
    IOLockWakeup(_lock, &_stateEvent, false);
}

/**
 * @internal
 * Used to track sendUSBDeviceRequest() completion context.
 */
struct CP210DeviceRequestHandler {
    /** The USB completion block */
    IOUSBCompletion completion;
    
    /** The USB request */
    IOUSBDevRequest req;

    /** pData buffer, if any. */
    uint8_t pData[];
};

/**
 * @internal
 *
 * Clean up the request data allocated in sendUSBDeviceRequest upon completion of an IOUSBDevRequest. 
 * @param target The IOMalloc-allocated IOUSBCompletion
 * @param paramter The IOMalloc-allocated IOUSBDevRequest
 */
void coop_plausible_driver_CP210x::sendUSBDeviceRequestCleanup (void *target, void *parameter, IOReturn status, UInt32 bufferSizeRemaining) {
    struct CP210DeviceRequestHandler *handler = (struct CP210DeviceRequestHandler *) parameter;
    IOUSBDevRequest *req = (IOUSBDevRequest *) &handler->req;

    if (status != kIOReturnSuccess)
        LOG_ERR("IOUSBDevRequest type=0x%x, request=0x%x returned error: 0x%x", (int) req->bmRequestType, (int) req->bRequest, status);

    IOFree(handler, sizeof(CP210DeviceRequestHandler) + req->wLength);
}


/**
 * Send a device request asynchronously, while managing our internal locking and maintaining re-entrancy safety.
 * This method will detect if the _stopping has been set and return an appropriate IOReturn.
 *
 * @param req The request to be sent. If req->pData is non-NULL, its contents will be copied
 * into a buffer that will be automatically deallocated upon completion (or error) of the asynchronous
 * call.
 *
 * @warning This method must be called with _lock held.
 */
IOReturn coop_plausible_driver_CP210x::sendUSBDeviceRequest (IOUSBDevRequest *req) {
    IOReturn ret;

    /* Allocate sufficient space for the structure, as well as any pData in the request */
    struct CP210DeviceRequestHandler *handler = (struct CP210DeviceRequestHandler *) IOMalloc(sizeof(CP210DeviceRequestHandler) + req->wLength);

    /* Copy in the request */
    bzero(&handler->req, sizeof(IOUSBDevRequest));

    handler->req.bmRequestType = req->bmRequestType;
    handler->req.bRequest = req->bRequest;
    handler->req.wValue = req->wValue;
    handler->req.wIndex = req->wIndex;
    handler->req.wLength = req->wLength;

    /* Copy the request pData out into a malloc'd buffer we control. */
    if (req->pData != NULL) {
        memcpy(handler->pData, req->pData, req->wLength);
        handler->req.pData = handler->pData;
    } else {
        handler->req.pData = NULL;
    }
    
    /* Allocate and initialize our completion handler */
    handler->completion.target = this;
    handler->completion.action = sendUSBDeviceRequestCleanup;
    handler->completion.parameter = handler;

    /* Issue our request. We unlock our mutex to avoid any chance of a dead-lock. */
    IOLockUnlock(_lock); {
        ret = _provider->GetDevice()->DeviceRequest(&handler->req, 5000, 0, &handler->completion);
    } IOLockLock(_lock);

    /* Perform cleanup if an immediate error occurs */
    if (ret != kIOReturnSuccess) {
        sendUSBDeviceRequestCleanup(this, &handler, ret, 0);
    }

    /* Verify that the driver has not been stopped */
    if (_stopping) {
        LOG_DEBUG("sendUSBDeviceRequest() - offline (stopping)");
        return kIOReturnOffline;
    }

    return ret;
}

// from IOSerialDriverSync
UInt32 coop_plausible_driver_CP210x::getState(void *refCon) {
    LOG_DEBUG("Get State");

    UInt32 res;
    IOLockLock(_lock); {    
        /* Verify that the driver has not been stopped */
        if (_stopping) {
            LOG_DEBUG("getState() - offline (stopping)");
            IOLockUnlock(_lock);
            return 0;
        }
        
        res = _state & STATE_EXTERNAL;
    }
    IOLockUnlock(_lock);

    return res;
}

/**
 * Directly set the internal state.
 *
 * @param state The updated state to be set.
 * @param mask The mask to use when setting @a state
 * @param refCon Reference constant.
 * @param haveLock If true, the method will assume that _lock is held. If false, the lock will be acquired
 * automatically.
 */
IOReturn coop_plausible_driver_CP210x::setState (UInt32 state, UInt32 mask, void *refCon, bool haveLock) {
    LOG_DEBUG("setState(0x%x, 0x%x, %p, %x)", state, mask, refCon, (uint32_t)haveLock);

    if (!haveLock) {
        IOLockLock(_lock);
        
        /* Verify that the driver has not been stopped while the lock was not held */
        if (_stopping) {
            LOG_DEBUG("setState() - offline (stopping)");
            IOLockUnlock(_lock);
            return kIOReturnOffline;
        }
    }

    /* If the port has neither been acquired, nor is it being acquired, then inform the caller that
     * the port is not open. */
    if ((_state & PD_S_ACQUIRED) == 0 && ((state & mask) & PD_S_ACQUIRED) == 0) {
        if (!haveLock)
            IOLockUnlock(_lock);
        return kIOReturnNotOpen;
    }
    
    /* Compute the new state, as well as the state delta */
    UInt32 newState = (_state & ~mask) | (state & mask);
    UInt32 deltaState = newState ^ _state;

    /* Write any control line changes */
    writeCP210xControlLineConfig(newState, mask);

    // TODO - Forbid state modifications we can't support via setState().
#if 0
    if (state & STATE_SET_UNSUPPORTED) {
        if (!haveLock)
            IOLockUnlock(_lock);
        return kIOReturnBadArgument;
    }
#endif

    /* Update the internal state */
    _state = newState;
    
    /* Check if any state in the delta is being observed in watchState, and if so,
     * trigger a wake on our condition lock. */
    if (deltaState & _watchState) {
        /* Reset the watchState -- it will be reinitialized by any woken threads that
         * are not satisfied by this updated state */
        _watchState = 0x0;
        
        /* Wake up all waiting threads. They will block on _lock until we release the
         * mutex (or, if haveLock is false, when our caller releases _lock). */
        IOLockWakeup(_lock, &_stateEvent, false);
    }

    if (!haveLock)
        IOLockUnlock(_lock);

    return kIOReturnSuccess;
}


// from IOSerialDriverSync
IOReturn coop_plausible_driver_CP210x::setState(UInt32 state, UInt32 mask, void *refCon) {
    return setState(state, mask, refCon, false);
}

/**
 * Block until the backing state matches @a state, masked by @a mask has changed.
 *
 * @param state The required state values to wait for.
 * @param mask The mask to use when evaluating @a state
 * @param refCon Reference constant.
 * @param haveLock If true, the method will assume that _lock is held. If false, the lock will be acquired
 * automatically.
 */
IOReturn coop_plausible_driver_CP210x::watchState (UInt32 *state, UInt32 mask, void *refCon, bool haveLock) {
    LOG_DEBUG("watchState(0x%x, 0x%x, %p, %d)", *state, mask, refCon, (int)haveLock);
    
    /* Ensure that state is non-NULL */
    if (state == NULL) {
        LOG_DEBUG("Watch request with NULL state");
        return kIOReturnBadArgument;
    }
    
    if (mask == 0) {
        LOG_DEBUG("Watch request with 0 mask");
        return kIOReturnSuccess;
    }

    /* Acquire our lock */
    if (!haveLock) {
        IOLockLock(_lock);

        /* Verify that the driver has not been stopped while the lock was not held */
        if (_stopping) {
            LOG_DEBUG("watchState() - offline (stopping)");
            IOLockUnlock(_lock);
            return kIOReturnOffline;
        }
    }
    
    /* Limit mask to EXTERNAL_MASK. There are no comments or documentation describing why this is
     * necessary, but this matches Apple's USBCDCDMM driver implementation. */
    mask &= STATE_EXTERNAL;

    /* There's nothing left to watch if the port has not been opened. */
    if ((_state & PD_S_ACQUIRED) == 0) {
        if (!haveLock)
            IOLockUnlock(_lock);
        return kIOReturnNotOpen;
    }
    
    UInt32 watchState = *state;
    
    /* To properly handle closure of the serial port, we must always track the PD_S_ACTIVE or PD_S_ACQUIRED state.
     * If the caller is not already doing so, we register our interest here. */
    bool autoActive = false;
    if ((mask & (PD_S_ACQUIRED | PD_S_ACTIVE)) == 0) {
        /* Watch for low on PD_S_ACTIVE bit */
        mask |= PD_S_ACTIVE;
        watchState &= ~PD_S_ACTIVE;

        /* Record that we enabled PD_S_ACTIVE monitoring */
        autoActive = true;
    }
    
    /* Loop (and block) until one of our watched state values is achieved */
    while (true) {
        /* Determine the currently matching states. We invert the current state mask with ~, and then use ^ to implement
         * XNOR. Truth table:
         *
         * X Y   O
         * 1 0 = 0
         * 0 0 = 1
         * 0 1 = 0
         * 1 1 = 1
         */
        UInt32 matched = (watchState ^ ~_state) & mask;
        if (matched != 0) {
            *state = _state & STATE_EXTERNAL;
            
            /* Ensure that we drop our lock before returning. No further access to internal
             * mutable state is required after this. */
            if (!haveLock)
                IOLockUnlock(_lock);

            /* If the port has been closed (and the caller was not tracking PD_S_ACTIVE),
             * return an error. Otherwise we're just informing the caller that PD_S_ACTIVE was set low,
             * closing the port. This must necessarily differ from success, as the caller's state changes
             * of interest have not been detected.
             */
            if (autoActive && (matched & PD_S_ACTIVE)) {
                return kIOReturnIOError;
            } else {
                return kIOReturnSuccess;
            }
        }

        /* Update the watched bits. This will reset the watchState on every loop -- it is reset to 0 when threads 
         * are signaled. */
        _watchState |= mask;
        
        /* Wait to be signaled on a state change */
        int rtn = IOLockSleep(_lock, &_stateEvent, THREAD_ABORTSAFE);
        if (rtn == THREAD_TIMED_OUT) {
            if (!haveLock)
                IOLockUnlock(_lock);
            return kIOReturnTimeout;
        } else if (rtn == THREAD_INTERRUPTED) {
            if (!haveLock)
                IOLockUnlock(_lock);
            return kIOReturnAborted;
        }

        /* Check if we've been stopped while the lock was relinquished. */
        if (_stopping) {
            LOG_DEBUG("watchState() - offline (stopping) after IOLockSleep");
            if (!haveLock)
                IOLockUnlock(_lock);
            return kIOReturnOffline;
        }
    }

    if (!haveLock)
        IOLockUnlock(_lock);

    /* Should not be reachable */
    LOG_ERR("Reached unreachable end of watchState()");
    return kIOReturnOffline;
}

// from IOSerialDriverSync
IOReturn coop_plausible_driver_CP210x::watchState(UInt32 *state, UInt32 mask, void *refCon) {
    return watchState(state, mask, refCon, false);
}

#pragma mark Configuration

// from IOSerialDriverSync
UInt32 coop_plausible_driver_CP210x::nextEvent(void *refCon) {
    IOReturn ret;

    /* This implementation matches AppleUSBCDCDMM, which doesn't
     * provide any event queueing. */
    IOLockLock(_lock); {
        if (_stopping) {
            LOG_DEBUG("nextEvent() - offline (stopping)");
            ret = kIOReturnOffline;
        } else if (_state & PD_S_ACTIVE) {
            ret = kIOReturnSuccess;
        } else {
            ret = kIOReturnNotOpen;
        }
    }
    IOLockUnlock(_lock);

    return ret;
}

// from IOSerialDriverSync
IOReturn coop_plausible_driver_CP210x::executeEvent(UInt32 event, UInt32 data, void *refCon) {
    IOReturn ret = kIOReturnSuccess;
    UInt32 stateUpdate;
    UInt32 stateMask;
    
    IOLockLock(_lock);

    /* Verify that the driver has not been stopped */
    if (_stopping) {
        LOG_DEBUG("executeEvent() - offline (stopping)");
        IOLockUnlock(_lock);
        return kIOReturnOffline;
    }
    
    switch (event) {
        case PD_E_ACTIVE: {
            LOG_DEBUG("executeEvent(PD_E_ACTIVE, %u, %p)", data, refCon);

            /* Start or stop the UART */
            bool starting = data;
            
            /* Skip if already started, already stopped */
            if (starting && (_state & PD_S_ACTIVE) != 0) {
                break;
            } else if (!starting && (_state & PD_S_ACTIVE) == 0) {
                break;
            }

            /* Set up the UART request */
            IOUSBDevRequest req;
            req.bmRequestType = USLCOM_WRITE;
            req.bRequest = USLCOM_IFC_ENABLE;
            req.wIndex = USLCOM_PORT_NO;
            req.wLength = 0;
            req.pData = NULL;

            stateMask = PD_S_ACTIVE;
            if (starting) {
                LOG_DEBUG("Enabling UART");
                stateUpdate = PD_S_ACTIVE;
                req.wValue = USLCOM_IFC_ENABLE_EN;
            } else {
                LOG_DEBUG("Disabling UART");
                stateUpdate = 0;
                req.wValue = USLCOM_IFC_ENABLE_DIS;
            }

            /* Issue request */
            ret = this->sendUSBDeviceRequest(&req);
            if (ret != kIOReturnSuccess) {
                /* Only log an error on start. At stop time, the device may have simply disappeared. */
                if (starting) {
                    LOG_ERR("Set PD_E_ACTIVE (data=%u) failed: %u", data, ret);
                    break;
                } else {
                    LOG_DEBUG("Ignoring PD_E_ACTIVE error %u on stop. The device was likely unplugged.", ret);
                    break;
                }
            }

            /* Update state */
            setState(stateUpdate, stateMask, refCon, true);
            
            // TODO - Restore any line state?
            
            break;
        }


        case PD_E_RXQ_SIZE:
            /* Adjust receive queue size. We're not required to support this. */
            LOG_DEBUG("executeEvent(PD_E_RXQ_SIZE, %u, %p)", data, refCon);
            break;
        case PD_E_TXQ_SIZE:
            /* Adjust send queue size. We're not required to support this. */
            LOG_DEBUG("executeEvent(PD_E_TXQ_SIZE, %u, %p)", data, refCon);
            break;
            
        case PD_E_RXQ_HIGH_WATER:
            /* Optional */
            LOG_DEBUG("executeEvent(PD_E_RXQ_HIGH_WATER, %u, %p)", data, refCon);
            break;

        case PD_E_RXQ_LOW_WATER:
            /* Optional */
            LOG_DEBUG("executeEvent(PD_E_RXQ_HIGH_WATER, %u, %p)", data, refCon);
            break;

        case PD_E_TXQ_HIGH_WATER:
            /* Optional */
            LOG_DEBUG("executeEvent(PD_E_TXQ_HIGH_WATER, %u, %p)", data, refCon);
            break;

        case PD_E_TXQ_LOW_WATER:
            /* Optional */
            LOG_DEBUG("executeEvent(PD_E_TXQ_LOW_WATER, %u, %p)", data, refCon);
            break;
            
        case PD_E_TXQ_FLUSH:
            /* No-op. */
            LOG_DEBUG("executeEvent(PD_E_TXQ_FLUSH, %u, %p)", data, refCon);
            break;
            
        case PD_E_RXQ_FLUSH:
            /* No-op. */
            LOG_DEBUG("executeEvent(PD_E_RXQ_FLUSH, %u, %p)", data, refCon);
            break;
            
        case PD_E_DATA_RATE: {
            /* Set the baud rate. */
            LOG_DEBUG("executeEvent(PD_E_DATA_RATE, %u>>1, %p)", data, refCon);
            
            /*
             * IOSerialBSDClient shifts the speed << 1 before issuing a PD_E_DATA_RATE,
             * claiming that the speed is stored in half-bits, but this does not appear
             * to be the case. Comments in Apple's serial drivers' PD_E_DATA_RATE merely
             * state 'For API compatiblilty with Intel' before reversing the shift.
             *
             * Summary: This is necessary to keep IOSerialBSDClient happy, and why
             * IOSerialBSDClient requires this is lost to the history of whatever
             * Apple engineer is responsible.
             */
            UInt32 baud = data >> 1;
            
            /* Set up the UART request */
            IOUSBDevRequest req;
            uint32_t reqBuad = OSSwapHostToLittleInt32(baud);
            req.bmRequestType = USLCOM_WRITE;
            req.bRequest = USLCOM_SET_BAUDRATE;
            req.wValue = 0;
            req.wIndex = USLCOM_PORT_NO;
            req.wLength = sizeof(reqBuad);
            req.pData = &reqBuad;
    
            /* Issue request */
            ret = this->sendUSBDeviceRequest(&req);
            if (ret == kIOReturnSuccess) {
                _baudRate = baud;
            } else {
                LOG_ERR("Set USLCOM_BAUD_RATE failed: %u", ret);
                break;
            }

            break;
        }
            
        case PD_E_RX_DATA_RATE:
            /* We don't support setting an independent RX data rate to anything but 0. It's unclear
             * why we need to support a value of zero, but this matches Apple's USBCDCDMM implementation. */
            LOG_DEBUG("executeEvent(PD_E_RX_DATA_RATE, %u>>1, %p)", data, refCon);
            if (data != 0)
                ret = kIOReturnBadArgument;
            break;
            
        case PD_E_DATA_INTEGRITY:
            // Fall-through
        case PD_E_RX_DATA_INTEGRITY:
            if (event == PD_E_DATA_INTEGRITY) {
                LOG_DEBUG("executeEvent(PD_E_DATA_INTEGRITY, %u, %p)", data, refCon);
            } else {
                LOG_DEBUG("executeEvent(PD_E_DATA_INTEGRITY, %u, %p)", data, refCon);
            }

            switch (data) {
                case PD_RS232_PARITY_NONE:
                case PD_RS232_PARITY_ODD:
                case PD_RS232_PARITY_EVEN:
                    /* Set TX+RX vs. RX-only parity */
                    if (event == PD_E_DATA_INTEGRITY) {
                        /* Attempt to write the new configuration */
                        ret = writeCP210xDataConfig(data, _twoStopBits, _characterLength);
                        if (ret == kIOReturnSuccess) {
                            /* Update internal state on success */
                            _txParity = data;
                            _rxParity = PD_RS232_PARITY_DEFAULT;
                        }

                    } else {
                        _rxParity = data;
                    }

                    break;
                    
                default:
                    /* Unsupported parity setting */
                    ret = kIOReturnBadArgument;
                    break;
            }

            break;
            
        case PD_RS232_E_STOP_BITS:
            /* Set the stop bits */
            LOG_DEBUG("executeEvent(PD_RS232_E_STOP_BITS, %u>>1, %p)", data, refCon);

            /* Provided as half bits */
            data >>= 1;
            bool newTwoStopBits;

            if (data == 1) {
                newTwoStopBits = false;
            } else if (data == 2) {
                newTwoStopBits = true;
            } else {
                LOG_ERR("PD_RS232_E_STOP_BITS with invalid data=%u", data);
                ret = kIOReturnBadArgument;
                break;
            }

            /* Attempt to write the new configuration */
            ret = writeCP210xDataConfig(_txParity, newTwoStopBits, _characterLength);
            if (ret == kIOReturnSuccess) {
                _twoStopBits = newTwoStopBits;
            }

            break;
            
        case PD_RS232_E_RX_STOP_BITS:
            /* We don't support setting an independent RX stop bit value to anything but 0. It's unclear
             * why we need to support a value of zero, but this matches Apple's USBCDCDMM implementation. */
            LOG_DEBUG("executeEvent(PD_RS232_E_RX_STOP_BITS, %u>>1, %p)", data, refCon);
            if (data != 0)
                ret = kIOReturnBadArgument;
            break;
            
        case PD_E_DATA_SIZE: {
            /* Set the character bit size */
            LOG_DEBUG("executeEvent(PD_E_DATA_SIZE, %u>>1, %p)", data, refCon);

            /* Provided as half bits */
            data >>= 1;
            
            if (data < 5 || data > 8) {
                ret = kIOReturnBadArgument;
                break;
            }

            /* Attempt to write the new configuration */
            ret = writeCP210xDataConfig(_txParity, _twoStopBits, data);
            if (ret == kIOReturnSuccess) {
                _characterLength = data;
            }

            break;
        }
            
        case PD_E_RX_DATA_SIZE:
            /* We don't support setting an independent RX data size to anything but 0. It's unclear
             * why we need to support a value of zero, but this matches Apple's USBCDCDMM implementation. */
            LOG_DEBUG("executeEvent(PD_E_RX_DATA_SIZE, %u>>1, %p)", data, refCon);
            if (data != 0)
                ret = kIOReturnBadArgument;
            break;

        case PD_E_FLOW_CONTROL:
            LOG_DEBUG("executeEvent(PD_E_FLOW_CONTROL, %x, %p)", data, refCon);

            /* Update state */
            ret = writeCP210xFlowControlConfig(_flowState);
            if (ret == kIOReturnSuccess)
                _flowState = data;

            break;

        case PD_RS232_E_XON_BYTE:
            LOG_DEBUG("executeEvent(PD_RS232_E_XON_BYTE, %u, %p)", data, refCon);
            _xonChar = data;
            break;
            
        case PD_RS232_E_XOFF_BYTE:
            LOG_DEBUG("executeEvent(PD_RS232_E_XOFF_BYTE, %u, %p)", data, refCon);
            _xoffChar = data;
            break;
            
        case PD_E_SPECIAL_BYTE:
            LOG_DEBUG("executeEvent(PD_E_SPECIAL_BYTE, %u, %p)", data, refCon);
            /**
             * 'Special' bytes are an optional optimization, used to implement
             * wake up of waiting threads if a 'special' character is received. This
             * is only used by the PPP and SLIP line disciplines. We do not support
             * this feature.
             */
            break;

        case PD_E_VALID_DATA_BYTE:
            LOG_DEBUG("executeEvent(PD_E_VALID_DATA_BYTE, %u, %p)", data, refCon);
            /**
             * Reset a 'special' byte set in PD_E_SPECIAL_BYTE.
             */
            break;

        default:
            LOG_DEBUG("Unsupported executeEvent(%x, %u, %p)", event, data, refCon);
            ret = kIOReturnBadArgument;
            break;
    }

    IOLockUnlock(_lock);
    return ret;
}

// from IOSerialDriverSync
IOReturn coop_plausible_driver_CP210x::requestEvent(UInt32 event, UInt32 *data, void *refCon) {
    IOReturn ret = kIOReturnSuccess;
    
    if (data == NULL) {
        LOG_DEBUG("requestEvent() NULL data argument");
        return kIOReturnBadArgument;
    }

    IOLockLock(_lock);

    /* Verify that the driver has not been stopped */
    if (_stopping) {
        LOG_DEBUG("requestEvent() - offline (stopping)");
        IOLockUnlock(_lock);
        return kIOReturnOffline;
    }

    switch (event) {
        case PD_E_ACTIVE:
            /* Return active status */
            if (_state & PD_S_ACTIVE) {
                *data = true;
            } else {
                *data = false;
            }
            
            LOG_DEBUG("requestEvent(PD_E_ACTIVE, %u, %p)", *data, refCon);
            break;
            
        case PD_E_RXQ_SIZE:
            /* Return rx buffer size */
            *data = _rxBuffer->getCapacity();

            LOG_DEBUG("requestEvent(PD_E_RXQ_SIZE, %u, %p)", *data, refCon);
            break;

        case PD_E_TXQ_SIZE:            
            /* Return tx buffer size */
            *data = _txBuffer->getCapacity();
            
            LOG_DEBUG("requestEvent(PD_E_TXQ_SIZE, %u, %p)", *data, refCon);
            break;
            
        case PD_E_RXQ_HIGH_WATER:
            /* Unsupported. */
            *data = 0;
            
            LOG_DEBUG("requestEvent(PD_E_RXQ_HIGH_WATER, %u, %p)", *data, refCon);
            break;
            
        case PD_E_RXQ_LOW_WATER:
            /* Unsupported. */
            *data = 0;
            
            LOG_DEBUG("requestEvent(PD_E_RXQ_HIGH_WATER, %u, %p)", *data, refCon);
            break;
            
        case PD_E_TXQ_HIGH_WATER:
            /* Unsupported. */
            *data = 0;
            
            LOG_DEBUG("requestEvent(PD_E_TXQ_HIGH_WATER, %u, %p)", *data, refCon);
            break;
            
        case PD_E_TXQ_LOW_WATER:
            /* Unsupported. */
            *data = 0;

            LOG_DEBUG("requestEvent(PD_E_TXQ_LOW_WATER, %u, %p)", *data, refCon);
            break;
            
            
        case PD_E_TXQ_AVAILABLE:
            /* Return the free space in the TX buffer */
            *data = _txBuffer->getCapacity() - _txBuffer->getLength();
            
            LOG_DEBUG("requestEvent(PD_E_TXQ_AVAILABLE, %u, %p)", *data, refCon);
            break;
            
        case PD_E_RXQ_AVAILABLE:
            /* Return the number of bytes available in the RX buffer */
            *data = _rxBuffer->getLength();
            
            LOG_DEBUG("requestEvent(PD_E_RXQ_AVAILABLE, %u, %p)", *data, refCon);
            break;
            
        case PD_E_DATA_RATE:
            /*
             * IOSerialBSDClient shifts the speed >>1 after receiving the PD_E_DATA_RATE data,
             * claiming that the speed is stored in half-bits; this is not actually the case.
             *
             * Comments in Apple's serial drivers' PD_E_DATA_RATE merely
             * state 'For API compatiblilty with Intel' before reversing the shift.
             *
             * Summary: This is necessary to keep IOSerialBSDClient happy, and why
             * IOSerialBSDClient requires this is lost to the history of whatever
             * Apple engineer is responsible.
             */
            *data = _baudRate << 1;
            LOG_DEBUG("requestEvent(PD_E_DATA_RATE, %u>>1, %p)", *data, refCon);
            break;
            
        case PD_E_RX_DATA_RATE:
            /* We don't support setting an independent RX data rate to anything but 0. It's unclear
             * why we need to return a value of zero, but this matches Apple's USBCDCDMM implementation. */
            *data = 0x0;
            LOG_DEBUG("requestEvent(PD_E_RX_DATA_RATE, %u, %p)", *data, refCon);
            break;
            
        case PD_E_DATA_INTEGRITY:
            /* Return the tx parity value */
            *data = _txParity;
            LOG_DEBUG("requestEvent(PD_E_DATA_INTEGRITY, %u, %p)", *data, refCon);
            break;
            
        case PD_E_RX_DATA_INTEGRITY:
            /* Return the rx parity value */
            *data = _rxParity;
            LOG_DEBUG("requestEvent(PD_E_RX_DATA_INTEGRITY, %u, %p)", *data, refCon);
            break;
            
        case PD_RS232_E_STOP_BITS:
            /* Return the stop bit value (required to be half-bits). */
            if (_twoStopBits) {
                *data = 2 << 1;
            } else {
                *data = 1 << 1;
            }
            LOG_DEBUG("requestEvent(PD_RS232_E_STOP_BITS, %u>>1, %p)", *data, refCon);
            break;
            
        case PD_RS232_E_RX_STOP_BITS:
            /* We don't support setting an independent RX stop bit value to anything but 0. It's unclear
             * why we need to return a value of zero, but this matches Apple's USBCDCDMM implementation. */
            *data = 0x0;
            LOG_DEBUG("requestEvent(PD_RS232_E_RX_STOP_BITS, %u, %p)", *data, refCon);
            break;
            
        case PD_E_DATA_SIZE: {
            /* Return the character bit length (required to be half-bits). */
            *data = _characterLength << 1;
            LOG_DEBUG("requestEvent(PD_E_DATA_SIZE, %u>>1, %p)", *data, refCon);
            break;
        }
            
        case PD_E_RX_DATA_SIZE: {
            /* Return the RX-specific character bit length. We only support 0x0. */
            *data = 0x00;
            LOG_DEBUG("requestEvent(PD_E_RX_DATA_SIZE, %u, %p)", *data, refCon);
            break;
        }

        case PD_E_FLOW_CONTROL:
            *data = _flowState & PL_RS232_A_FLOWCONTROL_MASK;
            LOG_DEBUG("requestEvent(PD_E_FLOW_CONTROL, %u, %p)", *data, refCon);
            break;

        case PD_RS232_E_XON_BYTE:            
            *data = _xonChar;
            LOG_DEBUG("requestEvent(PD_RS232_E_XON_BYTE, %u, %p)", *data, refCon);
            break;

        case PD_RS232_E_XOFF_BYTE:
            *data = _xoffChar;
            LOG_DEBUG("requestEvent(PD_RS232_E_XOFF_BYTE, %u, %p)", *data, refCon);
            break;
            
        default:
            LOG_DEBUG("Unsupported requestEvent(%x, %p, %p)", event, data, refCon);
            ret = kIOReturnBadArgument;
            break;
    }

    IOLockUnlock(_lock);
    return ret;
}

#pragma mark Event Queueing

// from IOSerialDriverSync
IOReturn coop_plausible_driver_CP210x::enqueueEvent(UInt32 event, UInt32 data, bool sleep, void *refCon) {
    return executeEvent(event, data, refCon);
}

// from IOSerialDriverSync
IOReturn coop_plausible_driver_CP210x::dequeueEvent(UInt32 *event, UInt32 *data, bool sleep, void *refCon) {
    /* Since we don't impelement event reporting from nextEvent(), this implementation is not required
     * to provide a queued event. We return success if the port is open and the arguments
     * are not invalid. */
    
    if (event == NULL || data == NULL)
        return kIOReturnBadArgument;
    
    if (getState(refCon) & PD_S_ACTIVE)
        return kIOReturnSuccess;
    
    return kIOReturnNotOpen;
}


#pragma mark RX/TX

// IOUSBCompletion transmit receive handler.
void coop_plausible_driver_CP210x::receiveHandler (void *target, void *parameter, IOReturn status, UInt32 bufferSizeRemaining) {
    /* MEMORY WARNING: If aborted, this reference may now be invalid */
    coop_plausible_driver_CP210x *me = (coop_plausible_driver_CP210x *) target;

    /* MEMORY WARNING: This was retained in startReceive(), and we are responsible for releasing it */
    IOBufferMemoryDescriptor *mem = (IOBufferMemoryDescriptor *) parameter;
    
    /* If our transfer was aborted, our reference to the target may no longer be valid */
    if (status == kIOReturnAborted) {
        mem->release();
        return;
    }

    IOLockLock(me->_lock);
    
    /* Verify that the driver has not been stopped while the lock was not held */
    if (me->_stopping) {
        LOG_DEBUG("receiveHandler() - offline (stopping)");
        mem->release();
        IOLockUnlock(me->_lock);
        return;
    }
    
    if (status == kIOUSBPipeStalled) {
        LOG_ERR("Read IOUSBPipe stalled, resetting.\n");
        me->_inputPipe->ClearPipeStall(true);
    }
    
    if (status == kIOReturnOverrun) {
        LOG_ERR("Read IOUSBPipe overran buffer, resetting. Lost data.\n");
    }

    /* Append the new data. This -must- fit, as the data request size is never larger than the
     * amount of available space in our RX buffer. */
    UInt32 length = (uint32_t) (mem->getLength() - bufferSizeRemaining);
    assert(length < UINT32_MAX);

    if (length > 0) {
        uint32_t written = me->_rxBuffer->write(mem->getBytesNoCopy(), length);
        if (written != length) {
            /* This should never happen */
            LOG_ERR("Bug in receiveHandler()! More bytes were received than buffer space exists. Lost %lu bytes", (unsigned long) (written - mem->getLength()));
        }
    }
    
    /* Clean up the memory buffer */
    mem->release();

    /* Update the queue state */
    me->updateRXQueueState(NULL);

    /* Mark ourselves as finished */
    me->setState(0, PD_S_RX_BUSY, NULL, true);

    /* Let startReceive determine if we need to receive again before we relinquish our lock. */
    me->startReceive(NULL);

    IOLockUnlock(me->_lock);
}

/**
 * Attempt to read any available bytes from our input IOUSBPipe to the receive buffer. If a receive operation is already in
 * progress, or the receive queue is full, this function will immediately return.
 *
 * @param refCon Reference constant.
 *
 * @warning Must be called with _lock held.
 */
IOReturn coop_plausible_driver_CP210x::startReceive (void *refCon) {
    /* Nothing to do if we're not active */
    if ((_state & PD_S_ACTIVE) == 0) {
        return kIOReturnNotOpen;
    }
    
    /* If a receive is already in progress, we have nothing to schedule. */
    if (_state & PD_S_RX_BUSY) {
        return kIOReturnSuccess;
    }
    
    /* If the receive queue is full, we have nothing to schedule. */
    if (_state & PD_S_RXQ_FULL) {
        assert(_rxBuffer->getLength() == _rxBuffer->getCapacity());
        return kIOReturnSuccess;
    }
    
    /* Otherwise, mark ourselves as busy */
    this->setState(PD_S_RX_BUSY, PD_S_RX_BUSY, refCon, true);

    /*
     * Prepare our read buffer, using the available _rxBuffer space as our buffer's maximum capacity.
     *
     * TODO: This can fail in the case where we have less than _inputMaxPacketSize space available in
     * the _rxBuffer:
     *
     *    http://lists.apple.com/archives/usb/2003/Jan/msg00061.html
     *
     *    Assuming that the maxPacketSize for your bulk pipe is 64 (very likely) and you request 27 bytes
     *    but the device has 35 bytes available, it will send a 35 byte packet which won't fit in your 27
     *    byte buffer resulting in a kIOReturnOverrun and an immediate stall error on the next read on that pipe.
     *    Unlike most other operating systems, you can [not] read an arbitrary number of bytes from a bulk pipe on
     *    Mac OS (9 or X) -- you should always round your requests up to a multiple of the maximum packet size and buffer
     *    any excess for the next call.
     *
     * To correctly handle this, we'll need to treat the _rxBuffer as full if it has less than _inputMaxPacketSize
     * space free. The BUFFER_SIZE usage must also be modified to ensure that the RX buffer is sized appropriately
     * relative to the maximum input packet size.
     *
     * MEMORY WARNING: except in case of error, this buffer is released in our completion handler.
     */
    IOReturn ret = kIOReturnSuccess;
    
    uint32_t bufferAvail = _rxBuffer->getCapacity() - _rxBuffer->getLength();
    uint32_t nread = _inputMaxPacketSize;
    if (_inputMaxPacketSize > bufferAvail)
        nread = bufferAvail;

    IOBufferMemoryDescriptor *mem = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionIn, nread);
    if ((ret = mem->prepare()) != kIOReturnSuccess) {
        LOG_ERR("IOReturn=%d in IOBufferMemoryDescriptor::prepare(), receive queue may now stall.", ret);

        /* Mark ourselves as no longer busy */
        this->setState(0, PD_S_RX_BUSY, refCon, true);
        
        /* Clean up the buffer */
        mem->release();

        return ret;
    }
    
    /* Issue our transmit request. We unlock our mutex to avoid any chance of a dead-lock, and retain
     * the outputPipe to ensure that it is not deallocated out from under us. */
    _receiveHandler.parameter = mem;

    IOUSBPipe *inputPipe = _inputPipe;
    inputPipe->retain();
    IOLockUnlock(_lock); {
        ret = inputPipe->Read(mem, READ_TIMEOUT, READ_TIMEOUT, &_receiveHandler);
    } IOLockLock(_lock);
    inputPipe->release();
    
    /* Verify that the driver was not been stopped while the lock was not held */
    if (_stopping) {
        LOG_DEBUG("requestEvent() - offline (stopping)");
        return kIOReturnOffline;
    }

    if (ret != kIOReturnSuccess) {
        LOG_ERR("IOReturn=%d in IOUSBPipe::Read(), receive queue may now stall.", ret);
        mem->release();
    }
    
    return ret;
}

// IOUSBCompletion transmit result handler.
void coop_plausible_driver_CP210x::transmitHandler (void *target, void *parameter, IOReturn status, UInt32 bufferSizeRemaining) {
    /* If our transfer was aborted, our reference to the target may no longer be valid */
    if (status == kIOReturnAborted)
        return;

    coop_plausible_driver_CP210x *me = (coop_plausible_driver_CP210x *) target;
    IOLockLock(me->_lock);
    
    /* If our transfer stalled, clear the stall now */
    if (status == kIOUSBPipeStalled) {
        LOG_ERR("Write IOUSBPipe stalled, resetting.\n");
        me->_outputPipe->ClearPipeStall(true);
    }

    /* Update the queue state */
    me->updateTXQueueState(NULL);

    /* Mark ourselves as finished */
    me->setState(0, PD_S_TX_BUSY, NULL, true);
    
    /* Let startTransmit determine if we need to transmit again before we relinquish our lock. */
    me->startTransmit(NULL);

    IOLockUnlock(me->_lock);
}

/**
 * Attempt to write all available bytes from the transmit queue to our output IOUSBPipe. If a transmission is already in
 * progress, it will automatically handle any new data appended to the transmit buffer after it has started, and this
 * function will immediately return.
 *
 * @param refCon Reference constant.
 *
 * @warning Must be called with _lock held.
 */
IOReturn coop_plausible_driver_CP210x::startTransmit (void *refCon) {
    /* Nothing to do if we're not active */
    if ((_state & PD_S_ACTIVE) == 0) {
        return kIOReturnNotOpen;
    }

    /* If a transmit is already in progress, we have nothing to schedule. */
    if (_state & PD_S_TX_BUSY) {
        return kIOReturnSuccess;
    }

    /* If the transmit queue is empty, we have nothing to schedule. */
    if (_state & PD_S_TXQ_EMPTY) {
        assert(_txBuffer->getLength() == 0);
        return kIOReturnSuccess;
    }

    /* Otherwise, mark ourselves as busy */
    this->setState(PD_S_TX_BUSY, PD_S_TX_BUSY, refCon, true);
    
    
    /* Prepare our IO buffer. */
    IOReturn ret = kIOReturnSuccess;

    IOBufferMemoryDescriptor *mem = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionOut, _outputMaxPacketSize);
    uint32_t nbytes = _txBuffer->read(mem->getBytesNoCopy(), _outputMaxPacketSize);
    mem->setLength(nbytes);

    if ((ret = mem->prepare()) != kIOReturnSuccess) {
        LOG_ERR("IOReturn=%d in IOBufferMemoryDescriptor::prepare(), lost %lu bytes from write queue", ret, (unsigned long) nbytes);

        
        /* Mark ourselves as no longer busy */
        this->setState(0, PD_S_TX_BUSY, refCon, true);
        
        /* Clean up the buffer */
        mem->release();

        return ret;
    }

    /* Issue our transmit request. We unlock our mutex to avoid any chance of a dead-lock, and retain
     * the outputPipe to ensure that it is not deallocated out from under us. */
    IOUSBPipe *outputPipe = _outputPipe;
    outputPipe->retain();
    IOLockUnlock(_lock); {
        ret = outputPipe->Write(mem, 1000, 1000, &_transmitHandler);
    } IOLockLock(_lock);
    outputPipe->release();
    
    /* Verify that the driver was not stopped while the lock was not held */
    if (_stopping) {
        LOG_DEBUG("requestEvent() - offline (stopping)");
        return kIOReturnOffline;
    }

    if (ret != kIOReturnSuccess) {
        LOG_ERR("IOReturn=%d in IOUSBPipe::Write(), lost %lu bytes from write queue", ret, (unsigned long) nbytes);
    }

    /* Clean up our IO buffer */
    mem->release();

    return ret;
}

/**
 * Update the PD_S_TXQ_* state flags based on the current state of the transmisson
 * queue.
 *
 * @param refCon Reference constant.
 *
 * @warning Must be called with _lock held.
 */
void coop_plausible_driver_CP210x::updateTXQueueState (void *refCon) {
    /*
     * Determine the current value for PD_S_TXQ_FULL|PD_S_TXQ_EMPTY bits, one of:
     * - Non-empty: 0
     * - Non-empty (and full): PD_S_TXQ_FULL
     * - Empty (and not full): PD_S_TXQ_EMPTY
     */
    if (_txBuffer->getLength() > 0) {
        if (_txBuffer->getLength() == _txBuffer->getCapacity()) {
            /* If full, mark as full, and clear the empty flags */
            this->setState(PD_S_TXQ_FULL, PD_S_TXQ_FULL|PD_S_TXQ_EMPTY, NULL, true);
        } else {
            /* Otherwise, simply clear the empty AND the full flags */
            this->setState(0, PD_S_TXQ_FULL|PD_S_TXQ_EMPTY, NULL, true);
        }
    } else {
        /* Buffer is now empty, so we ought to set the empty flag, clear the full flag. */
        this->setState(PD_S_TXQ_EMPTY, PD_S_TXQ_FULL|PD_S_TXQ_EMPTY, NULL, true);
    }
}

/**
 * Update the PD_S_RXQ_* state flags based on the current state of the receive
 * queue.
 *
 * @param refCon Reference constant.
 *
 * @warning Must be called with _lock held.
 */
void coop_plausible_driver_CP210x::updateRXQueueState (void *refCon) {    
    /*
     * Determine the current value for PD_S_RXQ_FULL|PD_S_RXQ_EMPTY bits, one of:
     * - Non-empty: 0
     * - Non-empty (and full): PD_S_RXQ_FULL
     * - Empty (and not full): PD_S_RXQ_EMPTY
     */
    if (_rxBuffer->getLength() > 0) {
        if (_rxBuffer->getLength() == _rxBuffer->getCapacity()) {
            /* If full, mark as full, and clear the empty flags */
            this->setState(PD_S_RXQ_FULL, PD_S_RXQ_FULL|PD_S_RXQ_EMPTY, NULL, true);
        } else {
            /* Otherwise, simply clear the empty AND the full flags */
            this->setState(0, PD_S_RXQ_FULL|PD_S_RXQ_EMPTY, NULL, true);
        }
    } else {
        /* Buffer is now empty, so we ought to set the empty flag, clear the full flag. */
        this->setState(PD_S_RXQ_EMPTY, PD_S_RXQ_FULL|PD_S_RXQ_EMPTY, NULL, true);
    }
}

// from IOSerialDriverSync
IOReturn coop_plausible_driver_CP210x::enqueueData(UInt8 *buffer, UInt32 size, UInt32 *count, bool sleep, void *refCon) {
    IOLockLock(_lock);
    
    /* Verify that the driver was not stopped while the lock was not held */
    if (_stopping) {
        LOG_DEBUG("requestEvent() - offline (stopping)");
        IOLockUnlock(_lock);
        return kIOReturnOffline;
    }

    /* Nothing to do if we're not active */
    if ((_state & PD_S_ACTIVE) == 0) {
        IOLockUnlock(_lock);
        return kIOReturnNotOpen;
    }

    /* Perform the write, looping if the caller has requested that we sleep until all bytes are written */
    *count = 0;
    while (*count < size) {
        uint32_t written = _txBuffer->write(buffer + *count, size - *count);
        *count += written;
        
        /* Update the transmission queue state */
        this->updateTXQueueState(refCon);

        /* If requested by the caller, and not all bytes have been written, sleep until the transmit queue is no
         * longer marked full, and then continue writing. */
        if (*count < size && sleep && _state & PD_S_TXQ_FULL) {
            UInt32 reqState = 0;
            IOReturn ret;
            if ((ret = this->watchState(&reqState, PD_S_TXQ_FULL, refCon, true)) != kIOReturnSuccess) {
                IOLockUnlock(_lock);
                return ret;
            }
            
            
        } else if (_state & PD_S_TXQ_FULL) {
            /* If sleep was not requested, and the buffer is full, we can only exit the write loop. */
            break;
        }
    }

    /* If data was written, enable transmission. */
    if (*count > 0) {
        this->startTransmit(refCon);
    }

    IOLockUnlock(_lock);

    return kIOReturnSuccess;
}

// from IOSerialDriverSync
IOReturn coop_plausible_driver_CP210x::dequeueData(UInt8 *buffer, UInt32 size, UInt32 *count, UInt32 min, void *refCon) {
    /* Sanity check the min value */
    if (min > size) {
        LOG_ERR("Called with a minimum required read size that exceeds the target buffer's total size");
        return kIOReturnBadArgument;
    }

    IOLockLock(_lock);
    
    /* Verify that the driver was not stopped while the lock was not held */
    if (_stopping) {
        LOG_DEBUG("requestEvent() - offline (stopping)");
        IOLockUnlock(_lock);
        return kIOReturnOffline;
    }

    /* Nothing to do if we're not active */
    if ((_state & PD_S_ACTIVE) == 0) {
        IOLockUnlock(_lock);
        return kIOReturnNotOpen;
    }
    
    /* Perform the read, looping if the caller has requested that we sleep until min bytes are written */
    *count = 0;
    while (*count < size) {
        uint32_t nread = _rxBuffer->read(buffer + *count, size - *count);
        *count += nread;
        
        /* Update the transmission queue state */
        this->updateRXQueueState(refCon);

        /* Our read may have freed sufficient space to allow additional USB read requests. */
        this->startReceive(refCon);

        /* If requested by the caller, and not all bytes have been read, wait until the receive queue is no
         * longer marked empty, and then continue reading. */
        if (*count < min && _state & PD_S_RXQ_EMPTY) {
            UInt32 reqState = 0;
            IOReturn ret;
            if ((ret = this->watchState(&reqState, PD_S_RXQ_EMPTY, refCon, true)) != kIOReturnSuccess) {
                IOLockUnlock(_lock);
                return ret;
            }
            
        } else if (_state & PD_S_RXQ_EMPTY) {
            /* If sleep was not requested, and the buffer is empty, we can only exit the read loop. */
            break;
        }
    }
    
    IOLockUnlock(_lock);
    
    return kIOReturnSuccess;
}

#pragma mark CP210x Configuration

/**
 * Write control line configuration to the device.
 *
 * @param controlState PD_RS232_S_RTS or PD_RS232_S_DTR configuration flags.
 * @param mask The mask to use when evaluating @a controlState
 *
 * @warning Must be called with _lock held.
 */
IOReturn coop_plausible_driver_CP210x::writeCP210xControlLineConfig (UInt32 controlState, UInt32 controlMask) {
    // TODO -- This will blindly overwrite the flow control configuration
    //
    // CRTSCTS currently sets DTR high automatically, and leaves RTS in the control
    // of the hardware.
    uint16_t ctl = 0x0;

    if (controlMask & PD_RS232_S_DTR) {
        LOG_DEBUG("Drive PD_RS232_S_DTR %s", (controlState & PD_RS232_S_DTR) ? "high" : "low");

        ctl |= OSSwapHostToLittleInt16(USLCOM_MHS_DTR_SET);
        if (controlState & PD_RS232_S_DTR)
            ctl |= OSSwapHostToLittleInt16(USLCOM_MHS_DTR_ON);
    }

    if (controlMask & PD_RS232_S_RTS) {
        LOG_DEBUG("Drive PD_RS232_S_RTS %s", (controlState & PD_RS232_S_RTS) ? "high" : "low");

        ctl |= OSSwapHostToLittleInt16(USLCOM_MHS_RTS_SET);
        if (controlState & PD_RS232_S_RTS)
            ctl |= OSSwapHostToLittleInt16(USLCOM_MHS_RTS_ON);
    }

    /* Set up the USB request */
    IOUSBDevRequest req;
    req.bmRequestType = USLCOM_WRITE;
    req.bRequest = USLCOM_SET_MHS;
    req.wValue = ctl;
    req.wIndex = USLCOM_PORT_NO;
    req.wLength = 0;
    req.pData = NULL;

    /* Issue request */
    IOReturn ret = this->sendUSBDeviceRequest(&req);
    if (ret != kIOReturnSuccess) {
        LOG_ERR("Set USLCOM_SET_MHS failed: %u", ret);
    }
    
    return ret;
}

/**
 * Write the flow control configuration to the device.
 *
 * @param flowState PD_RS232_A_* flow control configuration.
 *
 * @warning Must be called with _lock held.
 */
IOReturn coop_plausible_driver_CP210x::writeCP210xFlowControlConfig (UInt32 flowState) {
    LOG_DEBUG("writeCP210xFlowControlConfig(0x%X)", flowState);
    
    /* Initialize the request data */
    uint32_t flowctrl[4] = { 0, 0, 0, 0 };
    
    /* IXON */
    if (flowState & PD_RS232_A_TXO) {
        LOG_DEBUG("[FC] Enabling IXON");
        flowctrl[1] |= OSSwapHostToLittleInt32(USLCOM_FLOW_XON_ON);
    }
    
    /* IXANY */
    if (flowState & PD_RS232_A_XANY) {
        LOG_DEBUG("[FC] Enabling XANY [unsupported]");
        // TODO - The hardware doesn't support this directly. We can emulate this by calling SET_XON from
        // the data reception code path; if the hardware is waiting for XON, it will resume transmit.
        //
        // We'll have to see if there's a way to do this more effeciently than sending SET_XON for every
        // incoming read when IXANY is set.
    }
    
    /* IXOFF */
    if (flowState & PD_RS232_A_RXO) {
        LOG_DEBUG("[FC] Enabling IXOFF");
        flowctrl[1] |= OSSwapHostToLittleInt32(USLCOM_FLOW_XOFF_ON);
    }
    
    /* CRTS_IFLOW */
    if (flowState & PD_RS232_A_RFR) {
        LOG_DEBUG("[FC] Enabling CRTS_IFLOW");
        flowctrl[1] |= OSSwapHostToLittleInt32(USLCOM_FLOW_RTS_HS);
    }
    
    /* CCTS_OFLOW */
    if (flowState & PD_RS232_A_CTS) {
        LOG_DEBUG("[FC] Enabling CCTS_OFLOW");
        flowctrl[0] |= OSSwapHostToLittleInt32(USLCOM_FLOW_CTS_HS);
    }
    
    /* CDTR_IFLOW / DTR_ON */
    if (flowState & PD_RS232_A_DTR) {
        LOG_DEBUG("[FC] Enabling CDTR_IFLOW");
        flowctrl[0] |= OSSwapHostToLittleInt32(USLCOM_FLOW_DTR_HS);
    } else {
        LOG_DEBUG("[FC] Driving DTR high");
        /*
         * XXX: we default to DTR on unless DTR handshaking is requested. This matches the
         * FreeBSD driver behavior, but it's unclear if this is a well-defined behavior,
         * as we may be potentially resetting the DTR value specified previously.
         */
        flowctrl[0] |= OSSwapHostToLittleInt32(USLCOM_FLOW_DTR_ON);
    }
    
    /* Set up the USB request */
    IOUSBDevRequest req;
    req.bmRequestType = USLCOM_WRITE;
    req.bRequest = USLCOM_SET_FLOW;
    req.wValue = 0;
    req.wIndex = USLCOM_PORT_NO;
    req.wLength = sizeof(flowctrl);
    req.pData = flowctrl;
    
    /* Issue request */
    IOReturn ret = this->sendUSBDeviceRequest(&req);
    if (ret != kIOReturnSuccess) {
        LOG_ERR("Set USLCOM_SET_FLOWCTRL failed: %u", ret);
    }
    
    return ret;
}

/**
 * Write the stop bits, parity, and character length settings to the device.
 *
 * @param txParity The USLCOM_PARITY_* constant to be used to configure the device.
 * @param twoStopBits If true, set stop bits to 2. Otherwise 1.
 * @param charLength The character length. Must be a value >= 5, <= 8.
 */
IOReturn coop_plausible_driver_CP210x::writeCP210xDataConfig (uint32_t txParity, bool twoStopBits, uint32_t charLength) {
    LOG_DEBUG("Writing data config");
    uint16_t data = 0;
    
    /* Configure the bit field */
    if (twoStopBits) {
        data = USLCOM_STOP_BITS_2;
    } else {
        data = USLCOM_STOP_BITS_1;
    }
    
    if (txParity == PD_RS232_PARITY_ODD) {
        data |= USLCOM_PARITY_ODD;
    } else if (txParity == PD_RS232_PARITY_EVEN) {
        data |= USLCOM_PARITY_EVEN;
    } else {
        data |= USLCOM_PARITY_NONE;
    }
    
    if (charLength >= 5 && charLength <= 8) {
        data |= USLCOM_SET_DATA_BITS(charLength);
    } else {
        LOG_ERR("Incorrect character length value configured: %u", charLength);
    }
    
    /* Set up the USB request */
    IOUSBDevRequest req;
    req.bmRequestType = USLCOM_WRITE;
    req.bRequest = USLCOM_SET_LINE_CTL;
    req.wValue = data;
    req.wIndex = USLCOM_PORT_NO;
    req.wLength = 0;
    req.pData = NULL;
    
    /* Issue request */
    IOReturn ret = this->sendUSBDeviceRequest(&req);
    if (ret != kIOReturnSuccess) {
        LOG_ERR("Set USLCOM_DATA failed: %u", ret);
    }
    
    return ret;
}