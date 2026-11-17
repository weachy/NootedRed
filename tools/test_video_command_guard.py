#!/usr/bin/env python3
"""Run the host drivers' processCommand on the CPU with memcpy/fetchResource stubs.

No IOKit connection, GPU command submission or kernel code execution is performed.
The production patch planner and the actual endCommandStream error gate are used.
"""
import importlib.util
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

HARNESS = r'''
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>
#include <sys/mman.h>
using UInt8 = uint8_t;
using SInt32 = int32_t;
using SInt64 = int64_t;
using mach_vm_address_t = uint64_t;
PLANNER
unsigned checks=0, failures=0;
void check(bool ok, const char* name) {
    ++checks;
    if (!ok) { ++failures; printf("FAIL %s\n",name); }
}
std::vector<UInt8> read(const char* path) {
    std::ifstream f(path,std::ios::binary);
    return {(std::istreambuf_iterator<char>(f)),{}};
}
void rel32(UInt8* p, UInt8* target) {
    auto d=static_cast<SInt32>(target-(p+4));
    memcpy(p,&d,4);
}
bool run(const std::vector<UInt8>& fixture, uint64_t address, uint64_t fetch,
         const std::vector<UInt8>& gate, bool fixed, bool hasResource) {
    auto* mem=static_cast<UInt8*>(mmap(nullptr,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0));
    if(mem==MAP_FAILED) return false;
    memcpy(mem,fixture.data(),fixture.size());
    VideoCommandPatch::Plan p;
    if(!VideoCommandPatch::makePlan(mem,fixture.size(),address,fetch,p)) return false;
    if(fixed) rel32(mem+p.branch+2,mem+p.failure);
    // The only external calls executed by these cases are header memcpy and failed fetchResource.
    // Redirect their original rel32 calls into local CPU stubs in the same executable allocation.
    UInt8 copyStub[]={0x48,0xB8,0,0,0,0,0,0,0,0,0xFF,0xE0};
    const auto copyAddress=reinterpret_cast<uint64_t>(&memcpy);
    memcpy(copyStub+2,&copyAddress,8);
    memcpy(mem+0x700,copyStub,sizeof(copyStub));
    UInt8 fetchStub[]={0x31,0xC0,0xC3}; // return false
    memcpy(mem+0x720,fetchStub,sizeof(fetchStub));
    rel32(mem+0x81,mem+0x700);
    rel32(mem+p.branch-9,mem+0x720);
    // push rbx; mov rbx,rdi; [actual cmp/jne from endCommandStream]; return 1/0; pop rbx; ret
    UInt8 gateStub[]={0x53,0x48,0x89,0xFB,
        0x83,0xBB,0x44,0x06,0,0,0,0x0F,0x85,0,0,0,0,
        0xB8,1,0,0,0,0x5B,0xC3,0x31,0xC0,0x5B,0xC3};
    if(gate.size()!=13 || memcmp(gate.data(),gateStub+4,9)!=0) return false;
    memcpy(gateStub+4,gate.data(),13);
    memcpy(mem+0x740,gateStub,sizeof(gateStub));
    rel32(mem+0x740+13,mem+0x740+24);
    if(mprotect(mem,4096,PROT_READ|PROT_EXEC)!=0) return false;
    alignas(16) UInt8 context[0x1400]{};
    uint32_t command[256]{};
    command[2]=256; // available command words
    command[4]=128; // output/header words, initially unpublished
    command[7]=hasResource ? 1 : 0;
    command[8]=1;   // resource ID
    command[9]=0;   // upper address relocation index
    command[10]=129;// lower address relocation index
    command[11]=0;
    uint32_t output=0;
    using Process=void(*)(void*,uint32_t*,uint32_t,uint32_t*,uint32_t);
    reinterpret_cast<Process>(mem)(context,command,0,&output,256);
    const auto error=*reinterpret_cast<int32_t*>(context+0x644);
    const auto submit=reinterpret_cast<bool(*)(void*)>(mem+0x740)(context);
    printf("%s resource=%s error=%d output=%u submissionGate=%s\n",
           fixed?"patched":"stock",hasResource?"failed":"none",error,output,submit?"accept":"reject");
    const bool expected=hasResource && fixed ?
        error==-6 && output==0 && !submit :
        error==0 && output==128 && submit;
    munmap(mem,4096);
    return expected;
}
int main(int argc,char** argv) {
    if(argc!=5) return 2;
    auto code=read(argv[1]), gate=read(argv[2]);
    const uint64_t address=strtoull(argv[3],nullptr,16), fetch=strtoull(argv[4],nullptr,16);
    VideoCommandPatch::Plan p;
    check(VideoCommandPatch::makePlan(code.data(),code.size(),address,fetch,p),"host control-flow recognised");
    if(!p.branch) return 2;
    check(run(code,address,fetch,gate,false,true),"stock reproduces false success on failed resource");
    check(run(code,address,fetch,gate,true,true),"patch rejects failed resource at real submission gate");
    check(run(code,address,fetch,gate,false,false),"stock valid zero-resource control");
    check(run(code,address,fetch,gate,true,false),"patch preserves valid zero-resource control");
    auto patched=code;
    auto displacement=static_cast<SInt32>(p.failure-(p.branch+6));
    memcpy(patched.data()+p.branch+2,&displacement,4);
    VideoCommandPatch::Plan q;
    check(VideoCommandPatch::makePlan(patched.data(),patched.size(),address,fetch,q) && q.alreadyApplied,
          "already applied patch recognised");
    for(size_t offset : {size_t(0x3e),p.success,p.failure,p.branch-2,p.branch-10}) {
        auto corrupt=code; corrupt[offset]^=0x40;
        check(!VideoCommandPatch::makePlan(corrupt.data(),corrupt.size(),address,fetch,q),
              "changed control-flow rejected");
    }
    auto wrongTarget=code;
    auto bad=static_cast<SInt32>(p.success+1-(p.branch+6));
    memcpy(wrongTarget.data()+p.branch+2,&bad,4);
    check(!VideoCommandPatch::makePlan(wrongTarget.data(),wrongTarget.size(),address,fetch,q),
          "branch into middle of instruction rejected");
    // A padding NOP after ret is outside the guarded function body.
    for(size_t n=0;n<p.failure+18;++n)
        check(!VideoCommandPatch::makePlan(code.data(),n,address,fetch,q),"truncated function rejected");
    printf("checks=%u failures=%u\n",checks,failures);
    return failures?1:0;
}
'''


def main():
    spec = importlib.util.spec_from_file_location("kc", ROOT / "tools/gen_vcn_kernel_data.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    kc = module.KernelCollection(module.DEFAULT_KC)
    header = (ROOT / "NootedRed/VideoCommandPatch.hpp").read_text()
    planner = "namespace VideoCommandPatch {\n" + header.split("namespace VideoCommandPatch\n{", 1)[1].split(
        "    inline bool install(", 1)[0] + "\n}"
    with tempfile.TemporaryDirectory(prefix="nred-video-guard-") as directory:
        temp = Path(directory)
        source = temp / "test.cpp"
        source.write_text(HARNESS.replace("PLANNER", planner))
        subprocess.run(["xcrun", "clang++", "-std=c++17", "-O1", "-Wall", "-Wextra",
                        str(source), "-o", str(temp / "test")], check=True)
        for driver in ("5000", "6000"):
            kext = kc.kexts["com.apple.kext.AMDRadeonX" + driver]
            prefix = "__ZN35AMDRadeonX" + driver + "_AMDAccelVideoContext"
            process = kext.symbols[prefix + "14processCommandEPj16VideoCommandTypeS0_j"]
            fetch = kext.symbols[prefix + "13fetchResourceEP22_AMDVideoResourceParamP21_AMDVideoResourceInfo"]
            end = kext.symbols[prefix + "16endCommandStreamER24IOAccelCommandStreamInfo"]
            following = min(a for a, _ in kext.mangled_text if a > process)
            (temp / "code.bin").write_bytes(kext.read(process, following - process))
            (temp / "gate.bin").write_bytes(kext.read(end + 0x35, 13))
            print(f"X{driver} processCommand={process:#x} fetchResource={fetch:#x}", flush=True)
            subprocess.run([str(temp / "test"), str(temp / "code.bin"), str(temp / "gate.bin"),
                            f"{process:x}", f"{fetch:x}"], check=True)


if __name__ == "__main__":
    main()
