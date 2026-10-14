#!/usr/bin/env python3
"""
Bare-minimum CLI for ESP32-S3 MIDI testing.
Helps verify MIDI connectivity and control from a terminal.
"""

import sys
import argparse
import time

import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _deps import require  # noqa: E402

require("mido", "rtmidi")
import mido  # noqa: E402

SYSEX_MANUFACTURER_ID = 0x7D
SYSEX_FAMILY_ID = 0x01
SYSEX_MODEL_ID = 0x01
CMD_PARAM_STREAM = 0x01

def find_port(name, is_input=False):
    ports = mido.get_input_names() if is_input else mido.get_output_names()
    # Exact match first
    if name in ports:
        return name
    # Partial match
    for p in ports:
        if name.lower() in p.lower():
            return p
    print(f"Error: Could not find MIDI {'input' if is_input else 'output'} port matching '{name}'")
    list_ports()
    sys.exit(1)

def list_ports():
    print("Available MIDI Output Ports:")
    out_ports = mido.get_output_names()
    if out_ports:
        for p in out_ports:
            print(f"  - {p}")
    else:
        print("  (None)")
        
    print("\nAvailable MIDI Input Ports:")
    in_ports = mido.get_input_names()
    if in_ports:
        for p in in_ports:
            print(f"  - {p}")
    else:
        print("  (None)")

def send_note_test(port_query):
    port_name = find_port(port_query, False)
    print(f"Sending test note to {port_name}...")
    try:
        with mido.open_output(port_name) as out:
            note_on = mido.Message('note_on', note=60, velocity=100)
            out.send(note_on)
            print("Sent Note On (C4, velocity 100)")
            time.sleep(1.0)
            note_off = mido.Message('note_off', note=60, velocity=0)
            out.send(note_off)
            print("Sent Note Off (C4)")
    except Exception as e:
        print(f"Failed: {e}")

def send_param(port_query, param_id, value, slot=0):
    port_name = find_port(port_query, False)
    val = int(value)
    if not (0 <= val <= 16383):
        print("Value must be between 0 and 16383")
        return
        
    msb = (val >> 7) & 0x7F
    lsb = val & 0x7F
    flags = 0
    
    sysex_data = [
        SYSEX_MANUFACTURER_ID,
        SYSEX_FAMILY_ID,
        SYSEX_MODEL_ID,
        CMD_PARAM_STREAM,
        int(param_id),
        msb,
        lsb,
        slot,
        flags
    ]
    
    hex_str = " ".join([f"{x:02X}" for x in sysex_data])
    print(f"Sending SysEx to {port_name}: F0 {hex_str} F7")
    try:
        with mido.open_output(port_name) as out:
            msg = mido.Message('sysex', data=sysex_data)
            out.send(msg)
            print("Sent.")
    except Exception as e:
        print(f"Failed: {e}")

def monitor(port_query):
    port_name = find_port(port_query, True)
    print(f"Monitoring incoming MIDI on '{port_name}'... (Press Ctrl+C to stop)")
    try:
        with mido.open_input(port_name) as in_port:
            for msg in in_port:
                print(msg)
    except KeyboardInterrupt:
        print("\nStopped.")
    except Exception as e:
        print(f"Failed: {e}")

def main(argv=None):
    parser = argparse.ArgumentParser(description="Bare-minimum CLI for ESP32-S3 MIDI testing")
    subparsers = parser.add_subparsers(dest="command", required=True)
    
    # List command
    parser_list = subparsers.add_parser('list', help="List available MIDI ports")
    
    # Play command
    parser_play = subparsers.add_parser('play', help="Play a test note (C4)")
    parser_play.add_argument("port", help="MIDI output port name (or partial match)")
    
    # Param command
    parser_param = subparsers.add_parser('param', help="Send a SysEx parameter update")
    parser_param.add_argument("port", help="MIDI output port name (or partial match)")
    parser_param.add_argument("param_id", type=lambda x: int(x, 0), help="Parameter ID (e.g., 0x01 for cutoff)")
    parser_param.add_argument("value", type=int, help="14-bit value (0-16383)")
    parser_param.add_argument("--slot", type=int, default=0, help="Target slot (default 0)")
    
    # CC command
    parser_cc = subparsers.add_parser('cc', help="Send a Control Change message")
    parser_cc.add_argument("port", help="MIDI output port name (or partial match)")
    parser_cc.add_argument("cc_num", type=int, help="CC number (0-127)")
    parser_cc.add_argument("value", type=int, help="CC value (0-127)")
    
    # Monitor command
    parser_monitor = subparsers.add_parser('monitor', help="Monitor incoming MIDI messages")
    parser_monitor.add_argument("port", help="MIDI input port name (or partial match)")
    
    args = parser.parse_args(argv)
    
    if args.command == 'list':
        list_ports()
    elif args.command == 'play':
        send_note_test(args.port)
    elif args.command == 'param':
        send_param(args.port, args.param_id, args.value, args.slot)
    elif args.command == 'cc':
        port_name = find_port(args.port, False)
        try:
            with mido.open_output(port_name) as out:
                msg = mido.Message('control_change', control=args.cc_num, value=args.value)
                out.send(msg)
                print(f"Sent CC {args.cc_num} = {args.value} to {port_name}")
        except Exception as e:
            print(f"Failed: {e}")
    elif args.command == 'monitor':
        monitor(args.port)

if __name__ == '__main__':
    main()
