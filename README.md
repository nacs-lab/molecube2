# Molecube 2

This is a rewrite of the old molecube program as a daemon with simpler API
without having to deal with HTTP/CGI protocols.
All of the external communication will be done through a binary protocol on top of zmq.
This way, the web frontend does not have to be on the same machine anymore and
the frontend can also control multiple backend at the same time, all talking the same
zmq protocol.

## Protocol

### Sequence

* `run_seq`

    `[version: 4bytes]`
    `[bytecode: n]`

    The version number and the code are passed in as two different ZMQ messages.
    This will be how the experiment talks to the backend.

    Return 16 bytes ID that can be waited/operated on by `wait_seq` and `cancel_seq`.
    An ID of all bits set indicates error.
    The ID will be followed by 2 bytes indicating if there's any TTL and DDS overrides.
    The reply will be sent right away indicating that the sequence is ready to start
    or has started.

* `run_cmdlist`

    `[version: 4bytes]`
    `[cmd_list: n]`

    This is similar to `run_seq` but uses an uncompressed and simpler format which
    supports all operations.

* `wait_seq`

    `[id: 16bytes][state: 1byte]`

    Wait for a sequence to reach a specific `state`.
    Allowed values and their meanings for the `state` are,

    * `0` for flushed

        i.e. all commands sent to FPGA for execution.

    * `1` for finished

        i.e. all commands has finished execution.

    Return 1 byte. `0` for success, `1` for cancellation.
    If a sequence is already cancelled before the request is recieved, `0` could be returned.

* `cancel_seq`

    `[id: 16bytes (optional)]`

    Cancel one (or all) sequences.
    Return 1 bytes. `1` if no sequence are cancelled.
    `0` if at least one sequence may be cancelled
    (though the sequence may not response if it is already started).

* `state_id`

    No argument, return an incrementing 64bit ID followed by a 64bit process ID.
    The first bit of the id indicates if there's a sequence running,
    i.e. values are constantly changing and the rest 63 bits is the number of changes.
    requested after startup.

    The caller can use this to avoid polling for update too frequently.

### TTL

* `override_ttl`

    `[low mask: 4bytes][high mask: 4bytes][normal mask: 4bytes]`

    The three masks specify the changes to the high and low override masks to be made.
    The new high and low masks will be returned.
    `[0: 4bytes][0: 4bytes][0: 4bytes]` is an no-op and can be used to get the current masks.

* `set_ttl`

    `[low mask: 4bytes][high mask: 4bytes]`

    The two masks specify the channels to be turned on or off.
    This should have the same effect as running a sequence to turn a channel on/off.
    The new values will be returned including the override.
    `[0: 4bytes][0: 4bytes]` is an no-op and can be used to get the current values.

### DDS

* `override_dds`

    `[[[id: 1byte][val: 4bytes]] x n]`

    where the 1 byte `id` is `[chn_type: 2bits][chn_num: 6bits]` and a override value of
    `0xffffffff` means the override is disabled.
    Allowed `chn_type` values are:

    * `0`: for DDS frequency
    * `1`: for DDS amplitude
    * `2`: for DDS phase

    Return `0` on success. `1` on error.

* `get_override_dds`

    No arguments. Return the list of overrides specified in the same format as
    the argument of `override_dds`.

* `set_dds`

    `[[[id: 1byte][val: 4bytes]] x n]`

    Same as `override_dds`. But set the current value without enabling override.
    Return `0` on success. `1` on error.

* `get_dds`

    See `get_override_dds` except that values for all enabled DDS's are returned.
    An optional list of 1 byte channel id (see `override_dds` for format)
    can be included as argument for filtering.

* `reset_dds`

    `[chn_num: 1byte]`

    Reset the DDS. Returns `[0: 1byte]`.

### DAC

TODO

### Clock

* `set_clock`

    `[clock: 1byte]`

    Return `[0: 1byte]`.

* `get_clock`

    No argument. Return `[clock: 1byte]`

### Miscellaneous

* `get_startup`

    No argument. Return the startup cmdlist in text format (NUL terminated).

* `set_startup`

    `[text form cmdlist: n bytes]`

    Set the startup cmdlist. The sequence will be parsed immediately.
    If parsing succeeded, return `[0: 1 byte]`, otherwise,
    return `[1: 1 byte]` followed by a serialization of the `SyntaxError` object
    in the format:

        [message: n bytes NUL terminate]
        [line: n bytes NUL terminate]
        [lineno: 4bytes]
        [colnum: 4bytes]
        [colstart: 4bytes]
        [colend: 4bytes]

* `set_ttl_names`

    `[[chn: 1byte][name: n byte NUL terminate] x n]`

    Return `[0: 1byte]` if succeeded, `[1: 1 byte]` on error.

* `get_ttl_names`

    Return all TTL names in the same format as `set_ttl_names`

* `set_dds_names`

    Similar to `set_ttl_names`. Use the DDS instead of TTL channel number.

* `get_ttl_names`

    Similar to `get_ttl_names`. Use the DDS instead of TTL channel number.
