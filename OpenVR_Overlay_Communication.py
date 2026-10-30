import socket
import struct

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
server_address = ("localhost", 12345)
sock.bind(server_address)

while True:
    data, address = sock.recvfrom(4096)
    received_int = struct.unpack("!l", data)[0]
    message = received_int
    print(message)
