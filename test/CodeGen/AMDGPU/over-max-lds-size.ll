; RUN: not llc -march=amdgcn -mcpu=tahiti < %s 2>&1 | FileCheck -check-prefix=ERROR %s
; RUN: not llc -march=amdgcn -mcpu=hawaii < %s 2>&1 | FileCheck -check-prefix=ERROR %s
; RUN: not llc -march=amdgcn -mcpu=fiji < %s 2>&1 | FileCheck -check-prefix=ERROR %s

; ERROR: error: LDS size exceeds device maximum

@huge = internal unnamed_addr addrspace(3) global [100000 x i32] undef, align 4

define void @promote_alloca_size_256() {
entry:
  %v0 = getelementptr inbounds [100000 x i32], [100000 x i32] addrspace(3)* @huge, i32 0, i32 0
  store i32 0, i32 addrspace(3)* %v0
  ret void
}
