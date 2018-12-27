#!/bin/sh
grpc_cli call localhost:6000 --protofiles=control.proto Control.ConnectSourceToSink "source:$1, sink:$2"
