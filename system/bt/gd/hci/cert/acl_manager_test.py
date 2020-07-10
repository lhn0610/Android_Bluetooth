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

import os
import sys
import logging

from cert.gd_base_test import GdBaseTestClass
from cert.event_stream import EventStream
from cert.truth import assertThat
from google.protobuf import empty_pb2 as empty_proto
from hci.facade import acl_manager_facade_pb2 as acl_manager_facade
from neighbor.facade import facade_pb2 as neighbor_facade
from hci.facade import controller_facade_pb2 as controller_facade
from hci.facade import facade_pb2 as hci_facade
import bluetooth_packets_python3 as bt_packets
from bluetooth_packets_python3 import hci_packets
from cert.py_hci import PyHci
from cert.py_acl_manager import PyAclManager


class AclManagerTest(GdBaseTestClass):

    def setup_class(self):
        super().setup_class(dut_module='HCI_INTERFACES', cert_module='HCI')

    # todo: move into GdBaseTestClass, based on modules inited
    def setup_test(self):
        super().setup_test()
        self.cert_hci = PyHci(self.cert, acl_streaming=True)
        self.dut_acl_manager = PyAclManager(self.dut)

    def teardown_test(self):
        self.cert_hci.close()
        self.dut_acl_manager.close()
        super().teardown_test()

    def test_dut_connects(self):
        self.cert_hci.enable_inquiry_and_page_scan()
        cert_address = self.cert_hci.read_own_address()

        with self.dut_acl_manager.initiate_connection(cert_address) as dut_acl:
            cert_acl = self.cert_hci.accept_connection()
            cert_acl.send_first(
                b'\x26\x00\x07\x00This is just SomeAclData from the Cert')

            dut_acl.wait_for_connection_complete()

            dut_acl.send(
                b'\x29\x00\x07\x00This is just SomeMoreAclData from the DUT')

            assertThat(cert_acl).emits(
                lambda packet: b'SomeMoreAclData' in packet.data)
            assertThat(dut_acl).emits(
                lambda packet: b'SomeAclData' in packet.payload)

    def test_cert_connects(self):
        dut_address = self.dut.hci_controller.GetMacAddressSimple()
        self.dut.neighbor.EnablePageScan(
            neighbor_facade.EnableMsg(enabled=True))

        self.dut_acl_manager.listen_for_incoming_connections()
        self.cert_hci.initiate_connection(dut_address)

        dut_acl = self.dut_acl_manager.accept_connection()

        cert_acl = self.cert_hci.complete_connection()

        dut_acl.send(
            b'\x29\x00\x07\x00This is just SomeMoreAclData from the DUT')

        cert_acl.send_first(
            b'\x26\x00\x07\x00This is just SomeAclData from the Cert')

        assertThat(cert_acl).emits(
            lambda packet: b'SomeMoreAclData' in packet.data)
        assertThat(dut_acl).emits(
            lambda packet: b'SomeAclData' in packet.payload)

    def test_recombination_l2cap_packet(self):
        self.cert_hci.enable_inquiry_and_page_scan()
        cert_address = self.cert_hci.read_own_address()

        with self.dut_acl_manager.initiate_connection(cert_address) as dut_acl:
            cert_acl = self.cert_hci.accept_connection()
            cert_acl.send_first(b'\x06\x00\x07\x00Hello')
            cert_acl.send_continuing(b'!')
            cert_acl.send_first(b'\xe8\x03\x07\x00' + b'Hello' * 200)

            dut_acl.wait_for_connection_complete()

            assertThat(dut_acl).emits(
                lambda packet: b'Hello!' in packet.payload,
                lambda packet: b'Hello' * 200 in packet.payload).inOrder()
