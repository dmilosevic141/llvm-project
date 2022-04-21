;; C reproducer:
;;
;; void foo(int *restrict a, int *restrict b, int **restrict c, int **restrict d)
;; {
;;   for (int i = 0; i < 100; i++)
;;	 {
;;     a[i] = 0;
;;     b[i] = 0
;;     for (int j = 0; j < 100; j++)
;;     {
;;	     a[i] += c[i][j];
;;       b[i] += d[i][j];
;;     }
;;	 }
;; }

;; RUN: opt -loop-conditional-licm -S < %s | FileCheck %s

; CHECK-LABEL: foo
define dso_local void @foo(i32* noalias nocapture noundef %0, i32* noalias nocapture noundef %1, i32** noalias nocapture noundef readonly %2, i32** noalias nocapture noundef readonly %3) local_unnamed_addr #0 {
; CHECK-NEXT: br label %5
  br label %5

; CHECK:      5:
; CHECK-NEXT:   %6 = phi i32 [ 0, %4 ], [ %52, %51 ]
; CHECK-NEXT:   %7 = icmp ult i32 %6, 100
; CHECK-NEXT:   br i1 %7, label %9, label %8
5:                                                ; preds = %30, %4
  %6 = phi i32 [ 0, %4 ], [ %31, %30 ]
  %7 = icmp ult i32 %6, 100
  br i1 %7, label %9, label %8

; CHECK:      8:
; CHECK-NEXT:   ret void
8:                                                ; preds = %5
  ret void

; CHECK:      9:
; CHECK-NEXT:   %10 = zext i32 %6 to i64
; CHECK-NEXT:   %11 = getelementptr inbounds i32, i32* %0, i64 %10
; CHECK-NEXT:   store i32 0, i32* %11, align 4
; CHECK-NEXT:   %12 = getelementptr inbounds i32, i32* %1, i64 %10
; CHECK-NEXT:   store i32 0, i32* %12, align 4
; CHECK-NEXT:   [[LCLICM_DEPPTREND:%.*]] = getelementptr i32, i32* %0, i64 100
; CHECK-NEXT:   [[LCLICM_PTRTOINTEND:%.*]] = ptrtoint i32* [[LCLICM_DEPPTREND:%.*]] to i64
; CHECK-NEXT:   [[LCLICM_DEPPTRSTART:%.*]] = getelementptr i32, i32* %0, i64 0
; CHECK-NEXT:   [[LCLICM_PTRTOINTSTART:%.*]] = ptrtoint i32* [[LCLICM_DEPPTRSTART:%.*]] to i64
; CHECK-NEXT:   [[LCLICM_DEPPTREND1:%.*]] = getelementptr i32*, i32** %2, i64 10000
; CHECK-NEXT:   [[LCLICM_PTRTOINTEND2:%.*]] = ptrtoint i32** [[LCLICM_DEPPTREND1:%.*]] to i64
; CHECK-NEXT:   [[LCLICM_DEPPTRSTART3:%.*]] = getelementptr i32*, i32** %2, i64 0
; CHECK-NEXT:   [[LCLICM_PTRTOINTSTART4:%.*]] = ptrtoint i32** [[LCLICM_DEPPTRSTART3:%.*]] to i64
; CHECK-NEXT:   [[LCLICM_DEPPTREND5:%.*]] = getelementptr i32, i32* %1, i64 100
; CHECK-NEXT:   [[LCLICM_PTRTOINTEND6:%.*]] = ptrtoint i32* [[LCLICM_DEPPTREND5:%.*]] to i64
; CHECK-NEXT:   [[LCLICM_DEPPTRSTART7:%.*]] = getelementptr i32, i32* %1, i64 0
; CHECK-NEXT:   [[LCLICM_PTRTOINTSTART8:%.*]] = ptrtoint i32* [[LCLICM_DEPPTRSTART7:%.*]] to i64
; CHECK-NEXT:   [[LCLICM_DEPPTREND9:%.*]] = getelementptr i32*, i32** %3, i64 10000
; CHECK-NEXT:   [[LCLICM_PTRTOINTEND10:%.*]] = ptrtoint i32** [[LCLICM_DEPPTREND9:%.*]] to i64
; CHECK-NEXT:   [[LCLICM_DEPPTRSTART11:%.*]] = getelementptr i32*, i32** %3, i64 0
; CHECK-NEXT:   [[LCLICM_PTRTOINTSTART12:%.*]] = ptrtoint i32** [[LCLICM_DEPPTRSTART11:%.*]] to i64
; CHECK-NEXT:   %13 = icmp sgt i64 [[LCLICM_PTRTOINTSTART:%.*]], [[LCLICM_PTRTOINTEND2:%.*]]
; CHECK-NEXT:   %14 = icmp slt i64 [[LCLICM_PTRTOINTEND:%.*]], [[LCLICM_PTRTOINTSTART4:%.*]]
; CHECK-NEXT:   [[LCLICM_OR:%.*]] = or i1 %13, %14
; CHECK-NEXT:   %15 = icmp sgt i64 [[LCLICM_PTRTOINTSTART8:%.*]], [[LCLICM_PTRTOINTEND:%.*]]
; CHECK-NEXT:   %16 = icmp slt i64 [[LCLICM_PTRTOINTEND:%.*]], [[LCLICM_PTRTOINTSTART12:%.*]]
; CHECK-NEXT:   [[LCLICM_OR13:%.*]] = or i1 %15, %16
; CHECK-NEXT:   [[LCLICM_AND:%.*]] = and i1 [[LCLICM_OR:%.*]], [[LCLICM_OR13:%.*]]
; CHECK-NEXT:   br i1 [[LCLICM_AND:%.*]], label [[_SPLIT:%.*]], label [[_SPLIT_LD_CLONE:%.*]]

; CHECK:      .split.ld.clone:
; CHECK-NEXT:   [[_PROMOTED:%.*]] = load i32, i32* %11, align 4
; CHECK-NEXT:   [[_PROMOTED14:%.*]] = load i32, i32* %12, align 4
; CHECK-NEXT:   br label %17

; CHECK:      17:
; CHECK-NEXT:   %18 = phi i32 [ [[_PROMOTED14:%.*]], [[_SPLIT_LD_CLONE:%.*]] ], [ %32, %17 ]
; CHECK-NEXT:   %19 = phi i32 [ [[_PROMOTED:%.*]], [[_SPLIT_LD_CLONE:%.*]] ], [ %27, %17 ]
; CHECK-NEXT:   %20 = phi i32 [ 0, [[_SPLIT_LD_CLONE:%.*]] ], [ %33, %17 ]
; CHECK-NEXT:   %21 = icmp ult i32 %20, 100
; CHECK-NEXT:   %22 = getelementptr inbounds i32*, i32** %2, i64 %10
; CHECK-NEXT:   %23 = load i32*, i32** %22, align 8
; CHECK-NEXT:   %24 = zext i32 %20 to i64
; CHECK-NEXT:   %25 = getelementptr inbounds i32, i32* %23, i64 %24
; CHECK-NEXT:   %26 = load i32, i32* %25, align 4
; CHECK-NEXT:   %27 = add nsw i32 %19, %26
; CHECK-NEXT:   %28 = getelementptr inbounds i32*, i32** %3, i64 %10
; CHECK-NEXT:   %29 = load i32*, i32** %28, align 8
; CHECK-NEXT:   %30 = getelementptr inbounds i32, i32* %29, i64 %24
; CHECK-NEXT:   %31 = load i32, i32* %30, align 4
; CHECK-NEXT:   %32 = add nsw i32 %18, %31
; CHECK-NEXT:   %33 = add nuw nsw i32 %20, 1
; CHECK-NEXT:   br i1 %21, label %17, label [[_LD_LCSSA_LD_CLONE:%.*]]

; CHECK:      .ld-lcssa.ld.clone:
; CHECK-NEXT:   [[_LCSSA15:%.*]] = phi i32 [ %32, %17 ]
; CHECK-NEXT:   [[_LCSSA:%.*]] = phi i32 [ %27, %17 ]
; CHECK-NEXT:   store i32 [[_LCSSA15:%.*]], i32* %12, align 4
; CHECK-NEXT:   store i32 [[_LCSSA:%.*]], i32* %11, align 4
; CHECK-NEXT:   br label %51

; CHECK:      .split:
; CHECK-NEXT:   br label %34
9:                                                ; preds = %5
  %10 = zext i32 %6 to i64
  %11 = getelementptr inbounds i32, i32* %0, i64 %10
  store i32 0, i32* %11, align 4
  %12 = getelementptr inbounds i32, i32* %1, i64 %10
  store i32 0, i32* %12, align 4
  br label %13

; CHECK:       34:
; CHECK-NEXT:   %35 = phi i32 [ 0, [[_SPLIT:%.*]] ], [ %50, %34 ]
; CHECK-NEXT:   %36 = icmp ult i32 %35, 100
; CHECK-NEXT:   %37 = getelementptr inbounds i32*, i32** %2, i64 %10
; CHECK-NEXT:   %38 = load i32*, i32** %37, align 8
; CHECK-NEXT:   %39 = zext i32 %35 to i64
; CHECK-NEXT:   %40 = getelementptr inbounds i32, i32* %38, i64 %39
; CHECK-NEXT:   %41 = load i32, i32* %40, align 4
; CHECK-NEXT:   %42 = load i32, i32* %11, align 4
; CHECK-NEXT:   %43 = add nsw i32 %42, %41
; CHECK-NEXT:   store i32 %43, i32* %11, align 4
; CHECK-NEXT:   %44 = getelementptr inbounds i32*, i32** %3, i64 %10
; CHECK-NEXT:   %45 = load i32*, i32** %44, align 8
; CHECK-NEXT:   %46 = getelementptr inbounds i32, i32* %45, i64 %39
; CHECK-NEXT:   %47 = load i32, i32* %46, align 4
; CHECK-NEXT:   %48 = load i32, i32* %12, align 4
; CHECK-NEXT:   %49 = add nsw i32 %48, %47
; CHECK-NEXT:   store i32 %49, i32* %12, align 4
; CHECK-NEXT:   %50 = add nuw nsw i32 %35, 1
; CHECK-NEXT:   br i1 %36, label %34, label [[_LD_LCSSA:%.*]]

; CHECK:      .ld-lcssa:
; CHECK-NEXT:   br label %51
13:                                               ; preds = %13, %9
  %14 = phi i32 [ 0, %9 ], [ %29, %13 ]
  %15 = icmp ult i32 %14, 100
  %16 = getelementptr inbounds i32*, i32** %2, i64 %10
  %17 = load i32*, i32** %16, align 8
  %18 = zext i32 %14 to i64
  %19 = getelementptr inbounds i32, i32* %17, i64 %18
  %20 = load i32, i32* %19, align 4
  %21 = load i32, i32* %11, align 4
  %22 = add nsw i32 %21, %20
  store i32 %22, i32* %11, align 4
  %23 = getelementptr inbounds i32*, i32** %3, i64 %10
  %24 = load i32*, i32** %23, align 8
  %25 = getelementptr inbounds i32, i32* %24, i64 %18
  %26 = load i32, i32* %25, align 4
  %27 = load i32, i32* %12, align 4
  %28 = add nsw i32 %27, %26
  store i32 %28, i32* %12, align 4
  %29 = add nuw nsw i32 %14, 1
  br i1 %15, label %13, label %30

; CHECK:      51:
; CHECK-NEXT:   %52 = add nuw nsw i32 %6, 1
; CHECK-NEXT:   br label %5
30:                                               ; preds = %13
  %31 = add nuw nsw i32 %6, 1
  br label %5
}
