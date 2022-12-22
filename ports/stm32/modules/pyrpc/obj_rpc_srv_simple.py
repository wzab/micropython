"""
   The code below implements a simple  MessagePack-based RPC server using the simple TCP
   as transport. After the hello message the server expects the 4-byte length
   of the request followed by the MessagePack encapsulated RPC call.
   
   Written by Wojciech M. Zabolotny, wzab<at>ise.pw.edu.pl or wzab01<at>gmail.com
   13.04.2021

   The code is based on a post: "Simple object based RPC for Python",
   https://groups.google.com/g/alt.sources/c/QasPxJkKIUs/m/An1JnLooNCcJ
   https://www.funet.fi/pub/archive/alt.sources/2722.gz

   This code is published as PUBLIC DOMAIN  or under 
   Creative Commons Zero v1.0 Universal license (whichever better suits your needs).
"""
import time
import ustruct
import umsgpack as mp
import usocket
import os
import _thread
import network

CMD_MAXLEN = 1000

#Functions which process requests
def remote_mult(a,b):
    return a*b

def remote_div(a,b):
    print(a,b,a/b)
    return a/b

def cat_file(fname):
    f=open(fname,"rb")
    return f.read()

#Table of functions
func={
  'mult':remote_mult,
  'div':remote_div,
  'file':cat_file,
}


def handle(msg):
    try:
        obj = mp.unpackb(msg)
        if len(obj) != 2:
            raise Exception("Wrong number of RPC objects, should be 2: name and arguments")
        if isinstance(obj[1],tuple) or isinstance(obj[1],list):
            res=func[obj[0]](*obj[1])
        elif isinstance(obj[1],dict):
            res=func[obj[0]](**obj[1])
        else:
            raise Exception("Wrong type of arguments in RPC, should be list, tuple or dictionary")
        res = ("OK", res)
    except Exception as e:
        res=("error", str(e))
    return mp.packb(res)

class rpcsrv():
    def __init__(self):
        self.lan = network.LAN()
        self.lan.active(True)
        self.thread = None
        self.active = False

    def run(self,port):
        if self.thread is not None:
            raise(Exception("Can't start server twice!"))
        self.active = True
        self.thread = _thread.start_new_thread(self.dosrv,(port,))

    def stop(self):
        self.active = False

    def read(self,nbytes):
        res=b''
        try:
            while True:
                blen = len(res)
                if blen >= nbytes:
                    return res
                new = self.c.read(nbytes-blen)
                if not new:
                    return None
                res += new
        except Exception:
            return None
    def write(self,msg):
        blen = len(msg)
        while sent < blen:
            sent += self.c.write(msg[sent:])
        

    def recvmsg(self):
        # Get the length of the request
        length = self.read(4)
        if length is None:
            return None
        length = ustruct.unpack('>L',length)[0]
        if length > CMD_MAXLEN:
            self.sendmsg(mp.packb(('error','CMD too long')))
            return None
        msg = self.read(length)
        return msg
                
    def sendmsg(self,msg):
        length = ustruct.pack('>L',len(msg))
        self.c.write(length)
        self.c.write(msg)

    def dosrv(self,port):
        addr = usocket.getaddrinfo('0.0.0.0',port)[0][-1]
        s = usocket.socket(usocket.AF_INET, usocket.SOCK_STREAM)
        s.setsockopt(usocket.SOL_SOCKET, usocket.SO_REUSEADDR, 1)
        s.bind(addr)
        s.listen(1)
        while self.active:
            self.c, addr = s.accept()
            self.c.write(b'RPC srv 1.0\n')
            while True:
                cmd = self.recvmsg()
                if cmd is None:
                    break
                resp = handle(cmd)
                self.sendmsg(resp)
            self.c.close()             
        s.close()
 
if __name__ == "__main__":
    srv = rpcsrv()
    srv.run(9999)

