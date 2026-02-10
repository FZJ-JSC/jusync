#!/usr/bin/env python3
"""
Simple ZMQ ROUTER broker for testing ANARI USD Middleware client
Simulates HPC broker router to test worker count functionality
"""

import zmq
import time
import sys
import threading
import json
from datetime import datetime

class SimpleBrokerRouter:
    def __init__(self, endpoint="tcp://*:5556"):
        self.endpoint = endpoint
        self.context = zmq.Context()
        self.socket = None
        self.running = False
        self.worker_count = 5  # Simulate 5 workers
        self.workers = [
            (1, "worker1.hpc.local", "192.168.1.101"),
            (2, "worker2.hpc.local", "192.168.1.102"),
            (3, "worker3.hpc.local", "192.168.1.103"),
            (4, "worker4.hpc.local", "192.168.1.104"),
            (5, "worker5.hpc.local", "192.168.1.105")
        ]
        
    def start(self):
        """Start the broker router"""
        try:
            self.socket = self.context.socket(zmq.ROUTER)
            self.socket.setsockopt(zmq.LINGER, 0)
            self.socket.bind(self.endpoint)
            self.running = True
            
            print(f"🚀 Broker Router started on {self.endpoint}")
            print(f"📊 Simulating {self.worker_count} workers")
            print("📝 Supported commands:")
            print("  - GET_WORKERS (string protocol)")
            print("  - REQ_LIST_FILES (binary protocol)")
            print("  - REQ_GET_FILE (binary protocol)")
            print("  - REQ_WORKER_STATUS (binary protocol)")
            print("  - Simple text messages")
            print("\nPress Ctrl+C to stop\n")
            
            return True
        except Exception as e:
            print(f"❌ Failed to start broker: {e}")
            return False
    
    def stop(self):
        """Stop the broker router"""
        self.running = False
        if self.socket:
            self.socket.close()
        self.context.term()
        print("\n🛑 Broker stopped")
    
    def handle_get_workers(self, identity):
        """Handle GET_WORKERS string request"""
        # Build worker list string: "WORKER_LIST|rank1:hostname1:ip1;rank2:hostname2:ip2;..."
        worker_list = []
        for rank, hostname, ip in self.workers:
            worker_list.append(f"{rank}:{hostname}:{ip}")
        
        response = f"WORKER_LIST|{';'.join(worker_list)}"
        
        # Send response back to client
        self.socket.send(identity, zmq.SNDMORE)
        self.socket.send_string(response)
        
        print(f"📤 Sent worker list to {identity.hex()[:8]}: {len(self.workers)} workers")
        return True
    
    def handle_binary_request(self, identity, data):
        """Handle binary protocol requests (ANARI USD message format)"""
        try:
            if len(data) < 8:  # Minimum size for magic + message_type
                print(f"⚠️ Invalid binary message size: {len(data)} bytes")
                return False
            
            # Parse magic and message type
            magic = int.from_bytes(data[0:4], byteorder='little')
            message_type = int.from_bytes(data[4:8], byteorder='little')
            
            print(f"📦 Binary message from {identity.hex()[:8]}:")
            print(f"  Magic: 0x{magic:08X} ({'USDF' if magic == 0x55534446 else 'INVALID'})")
            print(f"  Type: {message_type}")
            
            # Handle different message types
            if magic == 0x55534446:  # "USDF"
                if message_type == 100:  # REQ_LIST_FILES
                    print("  📁 REQ_LIST_FILES request")
                    # Create response
                    response = self.create_file_list_response()
                    self.send_binary_response(identity, response)
                    return True
                elif message_type == 101:  # REQ_GET_FILE
                    print("  📄 REQ_GET_FILE request")
                    # Parse filename from request
                    if len(data) >= 272:  # ZmqFileRequest size
                        filename = data[16:272].split(b'\x00')[0].decode('utf-8', errors='ignore')
                        print(f"  Requested file: {filename}")
                        response = self.create_file_chunk_response(filename)
                        self.send_binary_response(identity, response)
                    return True
                elif message_type == 20:  # REQ_WORKER_STATUS
                    print("  👷 REQ_WORKER_STATUS request")
                    response = self.create_worker_status_response()
                    self.send_binary_response(identity, response)
                    return True
                else:
                    print(f"  ⚠️ Unsupported binary message type: {message_type}")
                    return False
            else:
                print(f"  ❌ Invalid magic number: 0x{magic:08X}")
                return False
                
        except Exception as e:
            print(f"❌ Error handling binary request: {e}")
            return False
    
    def create_file_list_response(self):
        """Create a binary file list response - returns (header, data) tuple"""
        # ZmqFileListResponse structure
        magic = 0x55534446  # "USDF"
        message_type = 200  # RESP_FILE_LIST
        request_id = 1
        source_rank = 0  # Broker rank
        file_count = 3
        
        # Create header (20 bytes)
        header = bytearray()
        header.extend(magic.to_bytes(4, 'little'))
        header.extend(message_type.to_bytes(4, 'little'))
        header.extend(request_id.to_bytes(4, 'little'))
        header.extend(source_rank.to_bytes(4, 'little', signed=True))
        header.extend(file_count.to_bytes(4, 'little'))
        
        # Create file data (file_count * 256 bytes)
        data = bytearray()
        filenames = ["test_model.usda", "texture.png", "scene.usdc"]
        for filename in filenames:
            filename_bytes = filename.encode('utf-8')
            data.extend(filename_bytes)
            data.extend(b'\x00' * (256 - len(filename_bytes)))
        
        return bytes(header), bytes(data)
    
    def create_file_chunk_response(self, filename):
        """Create a binary file chunk response"""
        # ZmqFileChunk structure
        magic = 0x55534446  # "USDF"
        message_type = 201  # RESP_FILE_CHUNK
        request_id = 1
        source_rank = 1
        file_size = 1024
        chunk_offset = 0
        chunk_size = 512
        
        response = bytearray()
        response.extend(magic.to_bytes(4, 'little'))
        response.extend(message_type.to_bytes(4, 'little'))
        response.extend(request_id.to_bytes(4, 'little'))
        response.extend(source_rank.to_bytes(4, 'little', signed=True))
        
        # Filename (256 bytes)
        filename_bytes = filename.encode('utf-8')[:255]
        response.extend(filename_bytes)
        response.extend(b'\x00' * (256 - len(filename_bytes)))
        
        response.extend(file_size.to_bytes(8, 'little'))
        response.extend(chunk_offset.to_bytes(8, 'little'))
        response.extend(chunk_size.to_bytes(4, 'little'))
        
        # Add dummy chunk data
        dummy_data = b'X' * chunk_size
        response.extend(dummy_data)
        
        return bytes(response)
    
    def create_worker_status_response(self):
        """Create a binary worker status response"""
        # ZmqWorkerStatusResponse structure
        magic = 0x55534446  # "USDF"
        message_type = 22  # RESP_WORKER_STATUS
        request_id = 1
        source_rank = 1
        worker_count = self.worker_count
        worker_status = 1  # Idle
        last_heartbeat = int(time.time())
        
        response = bytearray()
        response.extend(magic.to_bytes(4, 'little'))
        response.extend(message_type.to_bytes(4, 'little'))
        response.extend(request_id.to_bytes(4, 'little'))
        response.extend(source_rank.to_bytes(4, 'little', signed=True))
        response.extend(worker_count.to_bytes(4, 'little'))
        response.extend(worker_status.to_bytes(4, 'little'))
        response.extend(last_heartbeat.to_bytes(8, 'little'))
        
        # Hostname (64 bytes)
        hostname = "worker1.hpc.local".encode('utf-8')
        response.extend(hostname[:63])
        response.extend(b'\x00' * (64 - len(hostname)))
        
        # GPU info (128 bytes)
        gpu_info = "NVIDIA A100 80GB".encode('utf-8')
        response.extend(gpu_info[:127])
        response.extend(b'\x00' * (128 - len(gpu_info)))
        
        return bytes(response)
    
    def send_binary_response(self, identity, data):
        """Send binary response to client
        data can be either:
        - bytes: single message response (for file data)
        - tuple (header, data): two-part response (for file list)
        """
        if isinstance(data, tuple) and len(data) == 2:
            # Two-part response (header + data)
            header, file_data = data
            self.socket.send(identity, zmq.SNDMORE)
            self.socket.send(header, zmq.SNDMORE)
            self.socket.send(file_data)
            print(f"📤 Sent two-part response: header={len(header)} bytes, data={len(file_data)} bytes to {identity.hex()[:8]}")
        else:
            # Single message response
            self.socket.send(identity, zmq.SNDMORE)
            self.socket.send(data)
            print(f"📤 Sent binary response ({len(data)} bytes) to {identity.hex()[:8]}")
    
    def handle_text_message(self, identity, message):
        """Handle simple text messages"""
        print(f"💬 Text message from {identity.hex()[:8]}: {message}")
        
        # Send reply
        reply = f"ACK: {message}"
        self.socket.send(identity, zmq.SNDMORE)
        self.socket.send_string(reply)
        
        print(f"📤 Sent reply: {reply}")
        return True
    
    def run(self):
        """Main broker loop"""
        poller = zmq.Poller()
        poller.register(self.socket, zmq.POLLIN)
        
        while self.running:
            try:
                socks = dict(poller.poll(1000))  # 1 second timeout
                
                if self.socket in socks:
                    # Receive message parts
                    identity = self.socket.recv()
                    
                    # Check for empty delimiter frame (string protocol)
                    try:
                        empty_frame = self.socket.recv(zmq.RCVMORE)
                        if len(empty_frame) == 0:
                            # String protocol: next frame is the actual message
                            message = self.socket.recv_string()
                            if message == "GET_WORKERS":
                                self.handle_get_workers(identity)
                            else:
                                self.handle_text_message(identity, message)
                            continue
                        else:
                            # Binary message or single frame text
                            data = empty_frame
                            more = self.socket.getsockopt(zmq.RCVMORE)
                            if more:
                                # Multi-part binary message
                                while more:
                                    next_part = self.socket.recv()
                                    data += next_part
                                    more = self.socket.getsockopt(zmq.RCVMORE)
                                self.handle_binary_request(identity, data)
                            else:
                                # Single frame text message
                                message = data.decode('utf-8', errors='ignore')
                                self.handle_text_message(identity, message)
                    except zmq.ZMQError as e:
                        print(f"⚠️ ZMQ error: {e}")
                        continue
                        
            except KeyboardInterrupt:
                print("\n🛑 Received interrupt, shutting down...")
                self.running = False
                break
            except Exception as e:
                print(f"❌ Error in broker loop: {e}")
                time.sleep(1)  # Prevent tight loop on error

def main():
    import argparse
    
    parser = argparse.ArgumentParser(description="Simple ZMQ ROUTER broker for testing")
    parser.add_argument("--endpoint", default="tcp://*:5556", help="ZMQ endpoint to bind to")
    parser.add_argument("--workers", type=int, default=5, help="Number of simulated workers")
    
    args = parser.parse_args()
    
    broker = SimpleBrokerRouter(args.endpoint)
    broker.worker_count = args.workers
    
    # Update workers list based on count
    broker.workers = []
    for i in range(1, args.workers + 1):
        broker.workers.append((i, f"worker{i}.hpc.local", f"192.168.1.{100+i}"))
    
    if not broker.start():
        return 1
    
    try:
        broker.run()
    finally:
        broker.stop()
    
    return 0

if __name__ == "__main__":
    sys.exit(main())