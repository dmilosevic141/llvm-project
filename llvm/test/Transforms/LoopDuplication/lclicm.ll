;; C reproducer:
;;
;; extern long *a, b[100][8];
;;
;; void fn()
;; {
;;   for (long i = 0; i < 100; i++)
;;	 {
;;     a[i] = 0;
;;     for (int j = 0; j < 8; j++)
;;     {
;;	     a[i] += b[i][j];
;;     }
;;	 }
;; }

;; RUN: opt -loop-conditional-licm -S < %s | FileCheck %s

@a = external dso_local local_unnamed_addr global i64*, align 8
@b = external dso_local local_unnamed_addr global [100 x [8 x i64]], align 16

; CHECK-LABEL: fn
define dso_local void @fn() local_unnamed_addr {
; CHECK-NEXT:   br label %1
  br label %1

; CHECK:      1:
; CHECK-NEXT:   [[I:%.*]] = phi i64 [ 0, %0 ], [ %18, %17 ]
; CHECK-NEXT:   %2 = icmp ult i64 [[I:%.*]], 100
; CHECK-NEXT:   br i1 %2, label %4, label %3
1:                                                ; preds = %7, %0
  %i = phi i64 [ 0, %0 ], [ %8, %7 ]
  %2 = icmp ult i64 %i, 100
  br i1 %2, label %4, label %3

; CHECK:      3:
; CHECK-NEXT:   ret void
3:                                                ; preds = %1
  ret void

; CHECK:      4:
; CHECK-NEXT:   %5 = load i64*, i64** @a, align 8
; CHECK-NEXT:   %6 = getelementptr inbounds i64, i64* %5, i64 [[I:%.*]]
; CHECK-NEXT:   store i64 0, i64* %6, align 8
; CHECK-NEXT:   [[LCLICM_DEPPTREND:%.*]] = getelementptr i64, i64* %5, i64 100
; CHECK-NEXT:   [[LCLICM_PTRTOINTEND:%.*]] = ptrtoint i64* [[LCLICM_DEPPTREND:%.*]] to i64
; CHECK-NEXT:   [[LCLICM_DEPPTRSTART:%.*]] = getelementptr i64, i64* %5, i64 0
; CHECK-NEXT:   [[LCLICM_PTRTOINTSTART:%.*]] = ptrtoint i64* [[LCLICM_DEPPTRSTART:%.*]] to i64
; CHECK-NEXT:   [[LCLICM_DEPPTREND1:%.*]] = getelementptr [100 x [8 x i64]], [100 x [8 x i64]]* @b, i64 6400
; CHECK-NEXT:   [[LCLICM_PTRTOINTEND2:%.*]] = ptrtoint [100 x [8 x i64]]* [[LCLICM_DEPPTREND1:%.*]] to i64
; CHECK-NEXT:   [[LCLICM_DEPPTRSTART3:%.*]] = getelementptr [100 x [8 x i64]], [100 x [8 x i64]]* @b, i64 0
; CHECK-NEXT:   [[LCLICM_PTRTOINTSTART4:%.*]] = ptrtoint [100 x [8 x i64]]* [[LCLICM_DEPPTRSTART3:%.*]] to i64
; CHECK-NEXT:   %7 = icmp sgt i64 [[LCLICM_PTRTOINTSTART:%.*]], [[LCLICM_PTRTOINTEND2:%.*]]
; CHECK-NEXT:   %8 = icmp slt i64 [[LCLICM_PTRTOINTEND:%.*]], [[LCLICM_PTRTOINTSTART4:%.*]]
; CHECK-NEXT:   [[LCLICM_OR:%.*]] = or i1 %7, %8
; CHECK-NEXT:   br i1 [[LCLICM_OR:%.*]], label [[_SPLIT:%.*]], label [[_SPLIT_LD_CLONE:%.*]]

; CHECK:      .split.ld.clone:
; CHECK-NEXT:   [[_PROMOTED:%.*]] = load i64, i64* %6, align 8
; CHECK-NEXT:   br label %9

; CHECK:      9:
; CHECK-NEXT:   %10 = phi i64 [ [[_PROMOTED:%.*]], [[_SPLIT_LD_CLONE:%.*]] ], [ %14, %9 ]
; CHECK-NEXT:   [[J_LD_CLONE:%.*]] = phi i32 [ 0, [[_SPLIT_LD_CLONE:%.*]] ], [ %15, %9 ]
; CHECK-NEXT:   %11 = zext i32 [[J_LD_CLONE:%.*]] to i64
; CHECK-NEXT:   %12 = getelementptr inbounds [100 x [8 x i64]], [100 x [8 x i64]]* @b, i64 0, i64 [[I:%.*]], i64 %11
; CHECK-NEXT:   %13 = load i64, i64* %12, align 8
; CHECK-NEXT:   %14 = add nsw i64 %10, %13
; CHECK-NEXT:   %15 = add nuw nsw i32 [[J_LD_CLONE:%.*]]
; CHECK-NEXT:   %16 = icmp ult i32 %15, 8
; CHECK-NEXT:   br i1 %16, label %9, label [[_LD_LCSSA_LD_CLONE:%.*]]

; CHECK:      .ld-lcssa.ld.clone:
; CHECK-NEXT:   [[I_LCSSA:%.*]] = phi i64 [ %14, %9 ]
; CHECK-NEXT:   store i64 [[I_LCSSA:%.*]], i64* %6, align 8
; CHECK-NEXT:   br label %17

; CHECK:      .split:
; CHECK-NEXT:   br label %19

; CHECK:      .ld-lcssa:
; CHECK-NEXT:   br label %17
4:                                                ; preds = %1
  %5 = load i64*, i64** @a, align 8
  %6 = getelementptr inbounds i64, i64* %5, i64 %i
  store i64 0, i64* %6, align 8
  br label %9

; CHECK:      17:
; CHECK-NEXT:   %18 = add nuw nsw i64 [[I:%.*]], 1
; CHECK-NEXT:   br label %1
7:                                                ; preds = %9
  %8 = add nuw nsw i64 %i, 1
  br label %1

; CHECK:      19:
; CHECK-NEXT:   [[J:%.*]] = phi i32 [ 0, [[_SPLIT:%.*]] ], [ %25, %19 ]
; CHECK-NEXT:   %20 = zext i32 [[J:%.*]] to i64
; CHECK-NEXT:   %21 = getelementptr inbounds [100 x [8 x i64]], [100 x [8 x i64]]* @b, i64 0, i64 [[I:%.*]], i64 %20
; CHECK-NEXT:   %22 = load i64, i64* %21, align 8
; CHECK-NEXT:   %23 = load i64, i64* %6, align 8
; CHECK-NEXT:   %24 = add nsw i64 %23, %22
; CHECK-NEXT:   store i64 %24, i64* %6, align 8
; CHECK-NEXT:   %25 = add nuw nsw i32 [[J:%.*]], 1
; CHECK-NEXT:   %26 = icmp ult i32 %25, 8
; CHECK-NEXT:   br i1 %26, label %19, label [[_LD_LCSSA:%.*]]
9:                                                ; preds = %4, %9
  %j = phi i32 [ 0, %4 ], [ %15, %9 ]
  %10 = zext i32 %j to i64
  %11 = getelementptr inbounds [100 x [8 x i64]], [100 x [8 x i64]]* @b, i64 0, i64 %i, i64 %10
  %12 = load i64, i64* %11, align 8
  %13 = load i64, i64* %6, align 8
  %14 = add nsw i64 %13, %12
  store i64 %14, i64* %6, align 8
  %15 = add nuw nsw i32 %j, 1
  %16 = icmp ult i32 %15, 8
  br i1 %16, label %9, label %7
}
