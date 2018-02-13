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
    Changes to the bytecode may be needed to specify if the standard epilogue,
    which is only needed for experiment sequence and not for other test sequence,
    should be included.

    Return value will be a boolean indicating if the run succeeded.

* `run_seq_async`?

    `[version: 8bytes][bytecode: n]`

    This will be needed to support connecting multiple FPGA boxed together.
    Can be added later. The request should be the same as `run_seq` but
    the reply will be sent right after the sequence is ready to start.

    An ID will be returned that can be waited/operated on by `wait_seq` and `cancel_seq`.

* `wait_seq`?

    Wait for a sequence to finish.

* `cancel_seq`?

    Cancel one (or all) sequences.

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

TODO: reset?

* `overwrite_dds`

    `[[[id: 1byte][val: 4bytes]] x n]`

    where the 1 byte `id` is `[chn_type: 2bits][chn_num: 6bits]` and a overwrite value of
    `0xffffffff` means the overwrite is disabled.

* `get_overwrite_dds`

    No arguments. Return the list of overwrites specified in the same format as
    the argument of `overwrite_dds`.

* `set_dds`

    `[[[id: 1byte][val: 4bytes]] x n]`

    Same as `overwrite_dds`

* `get_dds`

    See `get_overwrite_dds` except that values for all enabled DDS's are returned.
    An optional list of 1 byte channel numbers can be included for filtering.

### DAC

TODO

### Clock

TODO
