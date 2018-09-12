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

    `[version: 8bytes][bytecode: n]`

    This will be how the experiment talks to the backend.

    Return positive 16 bytes ID that can be waited/operated on by `wait_seq` and `cancel_seq`.
    Negative ID indicates error.
    The ID will be optionally followed by a list of TTL and DDS overwrites if exists.
    If any overwrites is returned, the TTL overwrite will always be included as
    `[high_mask: 4bytes][low_mask: 4bytes]` which could be `[0: 4bytes][0: 4bytes]`.
    DDS overwrites follows the TTL overwrites using a format same as the one for the
    `overwrite_dds` request below.
    The reply will be sent right away indicating that the sequence is ready to start
    or has started.

* `run_cmdlist`

    `[version: 8bytes][cmd_list: n]`

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
    Return 1 bytes. `0` if no sequence are cancelled.
    `1` if at least one sequence may be cancelled
    (though the sequence may not response if it is already started).

* `state_id`

    No argument, return an incrementing 64bit ID followed by a 64bit process ID.
    The first bit of the id indicates if there's a sequence running,
    i.e. values are constantly changing and the rest 63 bits is the number of changes.
    requested after startup.

    The caller can use this to avoid polling for update too frequently.

### TTL

* `overwrite_ttl`

    `[high mask: 4bytes][low mask: 4bytes][normal mask: 4bytes]`

    The three masks specify the changes to the high and low overwrite masks to be made.
    The new high and low masks will be returned.
    `[0: 4bytes][0: 4bytes][0: 4bytes]` is an no-op and can be used to get the current masks.

* `set_ttl`

    `[high mask: 4bytes][low mask: 4bytes]`

    The two masks specify the channels to be turned on or off.
    This should have the same effect as running a sequence to turn a channel on/off.
    The new values will be returned including the overwrite.
    `[0: 4bytes][0: 4bytes]` is an no-op and can be used to get the current values.

### DDS

* `overwrite_dds`

    `[[[id: 1byte][val: 4bytes]] x n]`

    where the 1 byte `id` is `[chn_type: 2bits][chn_num: 6bits]` and a overwrite value of
    `0xffffffff` means the overwrite is disabled.

* `get_overwrite_dds`

    No arguments. Return the list of overwrites specified in the same format as
    the argument of `overwrite_dds`.

* `set_dds`

    `[[[id: 1byte][val: 4bytes]] x n]`

    Same as `overwrite_dds`. But set the current value without enabling overwrite.

* `get_dds`

    See `get_overwrite_dds` except that values for all enabled DDS's are returned.
    An optional list of 1 byte channel id (see `overwrite_dds` for format)
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
