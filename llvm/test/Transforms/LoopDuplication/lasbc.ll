;; C reproducer:
;; #include <stdint.h>
;;
;; extern long a[], b[], c, n;
;;
;; void foo()
;; {
;;	  for (int32_t i = 0; i < n; i++)
;;	  {
;;		  a[i] = b[i] * c;
;;	  }
;; }

;; RUN: opt -lasbc -S < %s | FileCheck %s

@n = external dso_local local_unnamed_addr global i64, align 8
@b = external dso_local local_unnamed_addr global [0 x i64], align 8
@c = external dso_local local_unnamed_addr global i64, align 8
@a = external dso_local local_unnamed_addr global [0 x i64], align 8

; CHECK-LABEL: foo
define dso_local void @foo() local_unnamed_addr {
; CHECK-NEXT: entry:
; CHECK-NEXT:   %0 = load i64, i64* @n, align 8
; CHECK-NEXT:   %1 = icmp sgt i64 %0, 0
; CHECK-NEXT:   [[MUL_LASBC:%.*]] = mul i64 %0, 8
; CHECK-NEXT:   %2 = icmp sle i64 [[MUL_LASBC:%.*]], 2147483647
; CHECK-NEXT:   [[AND_LASBC:%.*]] = and i1 %1, %2
; CHECK-NEXT:   %3 = icmp eq i1 [[AND_LASBC:%.*]], true
; CHECK-NEXT:   br i1 %3, label [[ENTRY_SPLIT:%.*]], label [[ENTRY_SPLIT_LD_CLONE:%.*]]

; CHECK:      entry.split.ld.clone:
; CHECK-NEXT:   br label [[FOR_COND_LD_CLONE:%.*]]

; CHECK:      for.cond.ld.clone:
; CHECK-NEXT:   [[I_0_LD_CLONE:%.*]] = phi i32 [ 0, [[ENTRY_SPLIT_LD_CLONE:%.*]] ], [ [[INC_LD_CLONE:%.*]], [[FOR_BODY_LD_CLONE:%.*]] ]
; CHECK-NEXT:   [[CONV_LD_CLONE:%.*]] = zext i32 [[I_0_LD_CLONE:%.*]] to i64
; CHECK-NEXT:   [[CMP_LD_CLONE:%.*]] = icmp sgt i64 %0, [[CONV_LD_CLONE:%.*]]
; CHECK-NEXT:   br i1 [[CMP_LD_CLONE:%.*]], label [[FOR_BODY_LD_CLONE:%.*]], label [[FOR_COND_CLEANUP_LD_LCSSA_LD_CLONE:%.*]]

; CHECK:      for.body.ld.clone:
; CHECK-NEXT:   [[ARRAYIDX_LD_CLONE:%.*]] = getelementptr inbounds [0 x i64], [0 x i64]* @b, i64 0, i64 [[CONV_LD_CLONE:%.*]]
; CHECK-NEXT:   %4 = load i64, i64* [[ARRAYIDX_LD_CLONE:%.*]], align 8
; CHECK-NEXT:   %5 = load i64, i64* @c, align 8
; CHECK-NEXT:   [[MUL_LD_CLONE:%.*]] = mul nsw i64 %5, %4
; CHECK-NEXT:   [[ARRAYIDX3_LD_CLONE:%.*]] = getelementptr inbounds [0 x i64], [0 x i64]* @a, i64 0, i64 [[CONV_LD_CLONE:%.*]]
; CHECK-NEXT:   store i64 [[MUL_LD_CLONE:%.*]], i64* [[ARRAYIDX3_LD_CLONE:%.*]], align 8
; CHECK-NEXT:   [[INC_LD_CLONE:%.*]] = add nuw nsw i32 [[I_0_LD_CLONE:%.*]], 1
; CHECK-NEXT:   br label [[FOR_COND_LD_CLONE:%.*]]

; CHECK:      for.cond.cleanup.ld-lcssa.ld.clone:
; CHECK-NEXT:   br label [[FOR_COND_CLEANUP:%.*]]

; CHECK:      entry.split:
; CHECK-NEXT:   br label [[FOR_COND:%.*]]
entry:
  %0 = load i64, i64* @n, align 8
  br label %for.cond

; CHECK:      for.cond:
; CHECK-NEXT:   [[I_0_N:%.*]] = phi i64 [ 0, [[ENTRY_SPLIT:%.*]] ], [ [[INC_LASBC:%.*]], [[FOR_BODY:%.*]] ]
; CHECK-NEXT:   [[CMP:%.*]] = icmp sgt i64 %0, [[I_0_N:%.*]]
; CHECK-NEXT:   br i1 [[CMP:%.*]], label [[FOR_BODY:%.*]], label [[FOR_COND_CLEANUP_LD_LCSSA:%.*]]
for.cond:                                         ; preds = %for.body, %entry
  %i.0 = phi i32 [ 0, %entry ], [ %inc, %for.body ]
  %conv = zext i32 %i.0 to i64
  %cmp = icmp sgt i64 %0, %conv
  br i1 %cmp, label %for.body, label %for.cond.cleanup

; CHECK:      for.body:
; CHECK-NEXT:   [[ARRAYIDX:%.*]] = getelementptr inbounds [0 x i64], [0 x i64]* @b, i64 0, i64 [[I_0_N:%.*]]
; CHECK-NEXT:   %6 = load i64, i64* [[ARRAYIDX:%.*]], align 8
; CHECK-NEXT:   %7 = load i64, i64* @c, align 8
; CHECK-NEXT:   [[MUL:%.*]] = mul nsw i64 %7, %6
; CHECK-NEXT:   [[ARRAYIDX3:%.*]] = getelementptr inbounds [0 x i64], [0 x i64]* @a, i64 0, i64 [[I_0_N:%.*]]
; CHECK-NEXT:   store i64 [[MUL:%.*]], i64* [[ARRAYIDX3:%.*]], align 8
; CHECK-NEXT:   [[INC_LASBC:%.*]] = add i64 [[I_0_N:%.*]], 1
; CHECK-NEXT:   br label [[FOR_COND:%.*]]

; CHECK:      for.cond.cleanup.ld-lcssa:
; CHECK-NEXT:   br label [[FOR_COND_CLEANUP:%.*]]
for.body:                                         ; preds = %for.cond
  %arrayidx = getelementptr inbounds [0 x i64], [0 x i64]* @b, i64 0, i64 %conv
  %1 = load i64, i64* %arrayidx, align 8
  %2 = load i64, i64* @c, align 8
  %mul = mul nsw i64 %2, %1
  %arrayidx3 = getelementptr inbounds [0 x i64], [0 x i64]* @a, i64 0, i64 %conv
  store i64 %mul, i64* %arrayidx3, align 8
  %inc = add nuw nsw i32 %i.0, 1
  br label %for.cond

; CHECK:      for.cond.cleanup:
; CHECK-NEXT:   ret void
for.cond.cleanup:                                 ; preds = %for.cond
  ret void
}
