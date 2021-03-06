; NOTE: Assertions have been autogenerated by utils/update_test_checks.py
; RUN: opt -print-predicateinfo -analyze < %s 2>&1 | FileCheck %s

declare void @foo(i1)
declare void @bar(i32)
declare void @llvm.assume(i1)

define void @testor(i32 %x, i32 %y) {
; CHECK-LABEL: @testor(
; CHECK-NEXT:    [[XZ:%.*]] = icmp eq i32 [[X:%.*]], 0
; CHECK-NEXT:    [[YZ:%.*]] = icmp eq i32 [[Y:%.*]], 0
; CHECK-NEXT:    [[Z:%.*]] = or i1 [[XZ]], [[YZ]]
; CHECK-NEXT:    br i1 [[Z]], label [[ONEOF:%.*]], label [[NEITHER:%.*]]
; CHECK:       oneof:
; CHECK-NEXT:    call void @foo(i1 [[XZ]])
; CHECK-NEXT:    call void @foo(i1 [[YZ]])
; CHECK-NEXT:    call void @bar(i32 [[X]])
; CHECK-NEXT:    call void @bar(i32 [[Y]])
; CHECK-NEXT:    ret void
; CHECK:       neither:
; CHECK:         [[Y_0:%.*]] = call i32 @llvm.ssa.copy.i32(i32 [[Y]])
; CHECK:         [[YZ_0:%.*]] = call i1 @llvm.ssa.copy.i1(i1 [[YZ]])
; CHECK:         [[X_0:%.*]] = call i32 @llvm.ssa.copy.i32(i32 [[X]])
; CHECK:         [[XZ_0:%.*]] = call i1 @llvm.ssa.copy.i1(i1 [[XZ]])
; CHECK-NEXT:    call void @foo(i1 [[XZ_0]])
; CHECK-NEXT:    call void @foo(i1 [[YZ_0]])
; CHECK-NEXT:    call void @bar(i32 [[X_0]])
; CHECK-NEXT:    call void @bar(i32 [[Y_0]])
; CHECK-NEXT:    call void @foo(i1 [[Z]])
; CHECK-NEXT:    ret void
;
  %xz = icmp eq i32 %x, 0
  %yz = icmp eq i32 %y, 0
  %z = or i1 %xz, %yz
  br i1 %z, label %oneof, label %neither
oneof:
;; Should not insert on the true edge for or
  call void @foo(i1 %xz)
  call void @foo(i1 %yz)
  call void @bar(i32 %x)
  call void @bar(i32 %y)
  ret void
neither:
  call void @foo(i1 %xz)
  call void @foo(i1 %yz)
  call void @bar(i32 %x)
  call void @bar(i32 %y)
  call void @foo(i1 %z)
  ret void
}
define void @testand(i32 %x, i32 %y) {
; CHECK-LABEL: @testand(
; CHECK-NEXT:    [[XZ:%.*]] = icmp eq i32 [[X:%.*]], 0
; CHECK-NEXT:    [[YZ:%.*]] = icmp eq i32 [[Y:%.*]], 0
; CHECK-NEXT:    [[Z:%.*]] = and i1 [[XZ]], [[YZ]]
; CHECK-NEXT:    br i1 [[Z]], label [[BOTH:%.*]], label [[NOPE:%.*]]
; CHECK:       both:
; CHECK:         [[Y_0:%.*]] = call i32 @llvm.ssa.copy.i32(i32 [[Y]])
; CHECK:         [[YZ_0:%.*]] = call i1 @llvm.ssa.copy.i1(i1 [[YZ]])
; CHECK:         [[X_0:%.*]] = call i32 @llvm.ssa.copy.i32(i32 [[X]])
; CHECK:         [[XZ_0:%.*]] = call i1 @llvm.ssa.copy.i1(i1 [[XZ]])
; CHECK-NEXT:    call void @foo(i1 [[XZ_0]])
; CHECK-NEXT:    call void @foo(i1 [[YZ_0]])
; CHECK-NEXT:    call void @bar(i32 [[X_0]])
; CHECK-NEXT:    call void @bar(i32 [[Y_0]])
; CHECK-NEXT:    ret void
; CHECK:       nope:
; CHECK-NEXT:    call void @foo(i1 [[XZ]])
; CHECK-NEXT:    call void @foo(i1 [[YZ]])
; CHECK-NEXT:    call void @bar(i32 [[X]])
; CHECK-NEXT:    call void @bar(i32 [[Y]])
; CHECK-NEXT:    call void @foo(i1 [[Z]])
; CHECK-NEXT:    ret void
;
  %xz = icmp eq i32 %x, 0
  %yz = icmp eq i32 %y, 0
  %z = and i1 %xz, %yz
  br i1 %z, label %both, label %nope
both:
  call void @foo(i1 %xz)
  call void @foo(i1 %yz)
  call void @bar(i32 %x)
  call void @bar(i32 %y)
  ret void
nope:
;; Should not insert on the false edge for and
  call void @foo(i1 %xz)
  call void @foo(i1 %yz)
  call void @bar(i32 %x)
  call void @bar(i32 %y)
  call void @foo(i1 %z)
  ret void
}
define void @testandsame(i32 %x, i32 %y) {
; CHECK-LABEL: @testandsame(
; CHECK-NEXT:    [[XGT:%.*]] = icmp sgt i32 [[X:%.*]], 0
; CHECK-NEXT:    [[XLT:%.*]] = icmp slt i32 [[X]], 100
; CHECK-NEXT:    [[Z:%.*]] = and i1 [[XGT]], [[XLT]]
; CHECK-NEXT:    br i1 [[Z]], label [[BOTH:%.*]], label [[NOPE:%.*]]
; CHECK:       both:
; CHECK:         [[XLT_0:%.*]] = call i1 @llvm.ssa.copy.i1(i1 [[XLT]])
; CHECK:         [[X_0:%.*]] = call i32 @llvm.ssa.copy.i32(i32 [[X]])
; CHECK:         [[X_0_1:%.*]] = call i32 @llvm.ssa.copy.i32(i32 [[X_0]])
; CHECK:         [[XGT_0:%.*]] = call i1 @llvm.ssa.copy.i1(i1 [[XGT]])
; CHECK-NEXT:    call void @foo(i1 [[XGT_0]])
; CHECK-NEXT:    call void @foo(i1 [[XLT_0]])
; CHECK-NEXT:    call void @bar(i32 [[X_0_1]])
; CHECK-NEXT:    ret void
; CHECK:       nope:
; CHECK-NEXT:    call void @foo(i1 [[XGT]])
; CHECK-NEXT:    call void @foo(i1 [[XLT]])
; CHECK-NEXT:    call void @foo(i1 [[Z]])
; CHECK-NEXT:    ret void
;
  %xgt = icmp sgt i32 %x, 0
  %xlt = icmp slt i32 %x, 100
  %z = and i1 %xgt, %xlt
  br i1 %z, label %both, label %nope
both:
  call void @foo(i1 %xgt)
  call void @foo(i1 %xlt)
  call void @bar(i32 %x)
  ret void
nope:
  call void @foo(i1 %xgt)
  call void @foo(i1 %xlt)
  call void @foo(i1 %z)
  ret void
}

define void @testandassume(i32 %x, i32 %y) {
; CHECK-LABEL: @testandassume(
; CHECK-NEXT:    [[XZ:%.*]] = icmp eq i32 [[X:%.*]], 0
; CHECK-NEXT:    [[YZ:%.*]] = icmp eq i32 [[Y:%.*]], 0
; CHECK-NEXT:    [[Z:%.*]] = and i1 [[XZ]], [[YZ]]
; CHECK:         [[TMP1:%.*]] = call i1 @llvm.ssa.copy.i1(i1 [[XZ]])
; CHECK:         [[TMP2:%.*]] = call i32 @llvm.ssa.copy.i32(i32 [[X]])
; CHECK:         [[TMP3:%.*]] = call i1 @llvm.ssa.copy.i1(i1 [[YZ]])
; CHECK:         [[TMP4:%.*]] = call i32 @llvm.ssa.copy.i32(i32 [[Y]])
; CHECK-NEXT:    call void @llvm.assume(i1 [[Z]])
; CHECK-NEXT:    br i1 [[Z]], label [[BOTH:%.*]], label [[NOPE:%.*]]
; CHECK:       both:
; CHECK:         [[DOT03:%.*]] = call i32 @llvm.ssa.copy.i32(i32 [[TMP4]])
; CHECK:         [[DOT02:%.*]] = call i1 @llvm.ssa.copy.i1(i1 [[TMP3]])
; CHECK:         [[DOT01:%.*]] = call i32 @llvm.ssa.copy.i32(i32 [[TMP2]])
; CHECK:         [[DOT0:%.*]] = call i1 @llvm.ssa.copy.i1(i1 [[TMP1]])
; CHECK-NEXT:    call void @foo(i1 [[DOT0]])
; CHECK-NEXT:    call void @foo(i1 [[DOT02]])
; CHECK-NEXT:    call void @bar(i32 [[DOT01]])
; CHECK-NEXT:    call void @bar(i32 [[DOT03]])
; CHECK-NEXT:    ret void
; CHECK:       nope:
; CHECK-NEXT:    call void @foo(i1 [[Z]])
; CHECK-NEXT:    ret void
;
  %xz = icmp eq i32 %x, 0
  %yz = icmp eq i32 %y, 0
  %z = and i1 %xz, %yz
  call void @llvm.assume(i1 %z)
  br i1 %z, label %both, label %nope
both:
  call void @foo(i1 %xz)
  call void @foo(i1 %yz)
  call void @bar(i32 %x)
  call void @bar(i32 %y)
  ret void
nope:
  call void @foo(i1 %z)
  ret void
}

;; Unlike and/or for branches, assume is *always* true, so we only match and for it
define void @testorassume(i32 %x, i32 %y) {
;
; CHECK-LABEL: @testorassume(
; CHECK-NEXT:    [[XZ:%.*]] = icmp eq i32 [[X:%.*]], 0
; CHECK-NEXT:    [[YZ:%.*]] = icmp eq i32 [[Y:%.*]], 0
; CHECK-NEXT:    [[Z:%.*]] = or i1 [[XZ]], [[YZ]]
; CHECK-NEXT:    call void @llvm.assume(i1 [[Z]])
; CHECK-NEXT:    br i1 [[Z]], label [[BOTH:%.*]], label [[NOPE:%.*]]
; CHECK:       both:
; CHECK-NEXT:    call void @foo(i1 [[XZ]])
; CHECK-NEXT:    call void @foo(i1 [[YZ]])
; CHECK-NEXT:    call void @bar(i32 [[X]])
; CHECK-NEXT:    call void @bar(i32 [[Y]])
; CHECK-NEXT:    ret void
; CHECK:       nope:
; CHECK-NEXT:    call void @foo(i1 [[Z]])
; CHECK-NEXT:    ret void
;
  %xz = icmp eq i32 %x, 0
  %yz = icmp eq i32 %y, 0
  %z = or i1 %xz, %yz
  call void @llvm.assume(i1 %z)
  br i1 %z, label %both, label %nope
both:
  call void @foo(i1 %xz)
  call void @foo(i1 %yz)
  call void @bar(i32 %x)
  call void @bar(i32 %y)
  ret void
nope:
  call void @foo(i1 %z)
  ret void
}
