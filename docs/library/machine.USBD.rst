.. currentmodule:: machine
.. _machine.USBD:

class USBD -- USB Device driver
===============================

.. note:: ``machine.USBD`` is currently only supported on the rp2 and samd
          ports, and is **not** enabled in the default MicroPython firmware
          build configuration.

USBD provides a low-level Python API for implementing USB device functions using
Python code. It assumes familiarity with the USB standard. It's not recommended
to use this API directly, the goal is for everyone to install the usbd module(s)
from micropython-lib and use these interfaces instead.

.. warning:: This functionality is very new and currently the usbd module is not
             yet merged into micropython-lib. It can be found `here on GitHub
             <https://github.com/micropython/micropython-lib/pull/558>`_.

Constructors
------------

.. class:: USBD()

   Construct a USBD object.

   .. note:: This object is a singleton, each call to this constructor
             returns the same object reference.

Methods
-------

.. method:: USBD.init(descriptor_device_cb=None, descriptor_config_cb=None, descriptor_string_cb=None, open_cb=None, reset_cb=None, control_xfer_cb=None, xfer_cb=None)

            Initializes the ``USBD`` singleton object with the following
            optional callback functions:

            - ``descriptor_device_cb()`` - Should return the USBD device
              descriptor as a bytes object or similar. If the callback is unset
              or fails to return a buffer then the :ref:`static device descriptor
              <USBD.static>` is sent to the host instead.

            - ``descriptor_config_cb()`` - Should return the USBD configuration
              descriptor as a bytes object or similar. If the callback is unset
              or fails to return a buffer then the :ref:`static device descriptor
              <USBD.static>` is sent to the host instead.

            - ``descriptor_string_cb(index)`` - Should return a bytes object or
               similar with a plain ASCII string corresponding to the requested
               string descriptor index value. Return ``None`` if there is no
               string descriptor with this index.

            - ``open_cb(interface_descriptor_view)`` - This callback is called
              when the USB host performs a Set Configuration request.
              ``interface_descriptor_view`` is a memoryview of the interface
              descriptor that the host is accepting, it references the same
              buffer previously returned by ``descriptor_config_cb()``. The
              memoryview is only valid until the callback function returns.

           - ``reset_cb()`` - This callback is called when the USB host performs
             a bus reset. Any in-progress transfers will never complete. The USB
             host will most likely proceed to re-enumerate the USB device by
             calling the descriptor callbacks.

           - ``control_xfer_cb(stage, request)`` - This callback is called one
             or more times for each USB control transfer (device Endpoint 0).
             ``stage`` is one of:

             - ``1`` for SETUP stage.
             - ``2`` for DATA stage.
             - ``3`` for ACK stage.

             The ``request`` parameter is a memoryview to read the USB control
             request data for the applicable stage. The memoryview is only valid
             until the callback function returns.

             Result of the callback should be one of the following values:

             - ``False`` to stall the endpoint and reject the transfer.
             - ``True`` to continue the transfer.
             - A buffer object to provide data for this stage of the transfer.
               This should be a writable buffer for an ``OUT`` direction transfer, or a
               readable buffer with data for an ``IN`` direction transfer.

           - ``xfer_cb(ep, result, xferred_bytes)`` - This callback is called
             whenever a non-control transfer submitted from ``submit_xfer()``
             completes.

             - ``ep`` is the Endpoint number for the completed transfer.
             - ``result`` is ``True`` if the transfer succeeded, ``False``
               otherwise.
             - ``xferred_bytes`` is the number of bytes successfully
               transferred. In the case of a "short" transfer, ``result`` is
               True and ``xferred_bytes`` will be smaller than the length of the
               buffer submitted for the transfer.

            .. note:: If a bus reset occurs, ``xfer_cb`` is not called for any
                      transfers that have not already completed.

.. method:: USBD.submit_xfer(ep, buffer)

            Submit a USB transfer on endpoint number ``ep``. ``buffer`` must be
            an object implementing the buffer interface, with read access for
            ``IN`` endpoints and write access for ``OUT`` endpoints.

            When the transfer completes, the ``xfer_cb`` callback is called (see
            above).

.. method:: USBD.reenumerate()

            Calling this function will schedule the USB device to disconnect
            from the host and then reconnect. This should cause the USB host
            to re-enumerate the device.

            Calling this function is necessary if the USB device configuration
            has changed, so that the host can read the new configuration.

            Re-enumeration does not happen immediately when the function runs,
            it will happen shortly after the function returns. Execution of the
            ``reset_cb`` and ``open_cb`` callbacks can be treated as indications
            that re-enumeration has commenced and completed, respectively.

            .. note:: When the USB device disconnects, it will interrupt the device's
                      USB-CDC serial port interface if it's currently in use.

.. method:: USBD.stall(ep, [stall])

            Calling this function gets or sets the STALL state of a device endpoint.

            ``ep`` is the number of the endpoint.

            If the optional ``stall`` parameter is set, this is a boolean flag
            for the STALL state.

            The return value is the current stall state of the endpoint (before
            any change made by this function).

            An endpoint that is set to STALL may remain stalled until this
            function is called again, or STALL may be cleared automatically by
            the USB host.

Constants
---------

.. _USBD.static:

.. data:: USBD.static

          This constant object holds the "static" descriptor data which is
          compiled into the MicroPython firmware. These are the descriptors
          which are used if the ``USBD`` object is not initialised, for example
          to provide the standard USB-CDC serial interface (which is implemented
          in C not Python).

          It is optional for the descriptors returned from ``USBD`` callbacks to
          include the "static" descriptor data. If not included, any "static"
          USB functions will effectively be disabled.

          .. note:: Interface numbers, endpoint addresses, and string descriptor
             indexes used by ``USBD`` callbacks must not conflict with values
             used by the "static" descriptors.

          Descriptor callbacks will use the "static" values as a fallback if a
          callback is missing or returns ``None``.

          - ``itf_max`` - One more than the highest bInterfaceNumber value used
            in the "static" configuration descriptor.
          - ``ep_max`` - One more than the highest bEndpointAddress value used
            in the "static" configuration descriptor. Does not include any
            ``IN`` flag bit (0x80).
          - ``str_max`` - One more than the highest string descriptor index
            value used by any "static" descriptor.
          - ``desc_device`` - ``bytes`` object containing the "static" USB device
            descriptor.
          - ``desc_cfg`` - ``bytes`` object containing the "static" USB configuration
            descriptor.

