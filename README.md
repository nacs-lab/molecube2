# Molecube 2

This is a rewrite of the old molecube program as a daemon with simpler API
without having to deal with HTTP/CGI protocols.
All of the external communication will be done through a binary protocol on top of zmq.
This way, the web frontend does not have to be on the same machine anymore and
the frontend can also control multiple backend at the same time, all talking the same
zmq protocol.
