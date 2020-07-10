#!/usr/bin/env python3
#
#   Copyright 2020 - The Android Open Source Project
#
#   Licensed under the Apache License, Version 2.0 (the "License");
#   you may not use this file except in compliance with the License.
#   You may obtain a copy of the License at
#
#       http://www.apache.org/licenses/LICENSE-2.0
#
#   Unless required by applicable law or agreed to in writing, software
#   distributed under the License is distributed on an "AS IS" BASIS,
#   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#   See the License for the specific language governing permissions and
#   limitations under the License.

from google.protobuf import empty_pb2 as empty_proto
from cert.event_stream import EventStream
from cert.event_stream import IEventStream
from cert.captures import HciCaptures
from cert.closable import Closable
from cert.closable import safeClose
from bluetooth_packets_python3 import hci_packets
from cert.truth import assertThat
from hci.facade import acl_manager_facade_pb2 as acl_manager_facade


class PyAclManagerAclConnection(IEventStream, Closable):

    def __init__(self, device, acl_stream, remote_addr, handle):
        self.device = device
        self.handle = handle
        # todo enable filtering after sorting out handles
        #self.our_acl_stream = FilteringEventStream(acl_stream, None)
        self.our_acl_stream = acl_stream

        if remote_addr:
            remote_addr_bytes = bytes(
                remote_addr,
                'utf8') if type(remote_addr) is str else bytes(remote_addr)
            self.connection_event_stream = EventStream(
                self.device.hci_acl_manager.CreateConnection(
                    acl_manager_facade.ConnectionMsg(
                        address_type=int(
                            hci_packets.AddressType.PUBLIC_DEVICE_ADDRESS),
                        address=remote_addr_bytes)))
        else:
            self.connection_event_stream = None

    def close(self):
        safeClose(self.connection_event_stream)

    def wait_for_connection_complete(self):
        connection_complete = HciCaptures.ConnectionCompleteCapture()
        assertThat(self.connection_event_stream).emits(connection_complete)
        self.handle = connection_complete.get().GetConnectionHandle()

    def send(self, data):
        self.device.hci_acl_manager.SendAclData(
            acl_manager_facade.AclData(handle=self.handle, payload=bytes(data)))

    def get_event_queue(self):
        return self.our_acl_stream.get_event_queue()


class PyAclManager(Closable):

    def __init__(self, device):
        self.device = device

        self.acl_stream = EventStream(
            self.device.hci_acl_manager.FetchAclData(empty_proto.Empty()))
        self.incoming_connection_stream = None

    def close(self):
        safeClose(self.acl_stream)
        safeClose(self.incoming_connection_stream)

    # temporary, until everyone is migrated
    def get_acl_stream(self):
        return self.acl_stream

    def listen_for_incoming_connections(self):
        self.incoming_connection_stream = EventStream(
            self.device.hci_acl_manager.FetchIncomingConnection(
                empty_proto.Empty()))

    def initiate_connection(self, remote_addr):
        return PyAclManagerAclConnection(self.device, self.acl_stream,
                                         remote_addr, None)

    def accept_connection(self):
        connection_complete = HciCaptures.ConnectionCompleteCapture()
        assertThat(self.incoming_connection_stream).emits(connection_complete)
        handle = connection_complete.get().GetConnectionHandle()
        return PyAclManagerAclConnection(self.device, self.acl_stream, None,
                                         handle)
