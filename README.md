# RDMA Application
## Capabilities 
 * Read
 * Write
 * Send/Receive

## Supported Transport Modes
 * Reliable Connected (RC)
 * Unreliable Connected (UC)[^1]
 * Unreliable Datagram (UD)[^2]

 [^1]: Supports Write and Send/Receive Only

 [^2]: Supports Send/Receive Only


## Usage
```bash
$ ./rdma_app -g 1 -q <rc/uc/ud>               # Server Side
$ ./rdma_app -g 1 -q <rc/uc/ud> <server-ip>   # Client Side
```
