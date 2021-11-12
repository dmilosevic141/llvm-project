;; C reproducer:
;; int u;
;;
;; void f(int a[restrict], int n)
;; {
;;      for (int i = 0; i < n; ++i)
;;          if (a[i])
;;              ++u;
;; }

; RUN: opt -licm -licm-conditional-access-promotion -S < %s | FileCheck %s

@u = dso_local local_unnamed_addr global i32 0, align 4

; CHECK-LABEL: @f
define dso_local void @f(i32* noalias nocapture readonly %a, i32 %n) {

; CHECK-NEXT: entry:
; CHECK-NEXT:   [[U_PROMOTED:%.*]] = load i32, i32* @u, align 4
; CHECK-NEXT:   br label [[FOR_COND:%.*]]
entry:
  br label %for.cond

; CHECK:        for.cond:
; CHECK-NEXT:       [[U_FLAG4:%.*]] = phi i1 [ false, [[ENTRY:%.*]] ], [ [[U_FLAG:%.*]], [[FOR_INC:%.*]] ]
; CHECK-NEXT:       [[INC3:%.*]] = phi i32 [ [[U_PROMOTED:%.*]], [[ENTRY:%.*]] ], [ [[INC2:%.*]], [[FOR_INC:%.*]] ]
; CHECK-NEXT:       [[I_0:%.*]] = phi i32 [ 0, [[ENTRY:%.*]] ], [ [[INC1:%.*]], [[FOR_INC:%.*]] ]
; CHECK-NEXT:       [[CMP:%.*]] = icmp slt i32 [[I_0:%.*]], [[N:%.*]]
; CHECK-NEXT:       br i1 [[CMP:%.*]], label [[FOR_BODY:%.*]], label [[FOR_COND_CLEANUP:%.*]]
for.cond:                                         ; preds = %for.inc, %entry
  %i.0 = phi i32 [ 0, %entry ], [ %inc1, %for.inc ]
  %cmp = icmp slt i32 %i.0, %n
  br i1 %cmp, label %for.body, label %for.cond.cleanup

; CHECK:        for.body:
; CHECK-NEXT:       [[IDXPROM:%.*]] = zext i32 [[I_0:%.*]] to i64
; CHECK-NEXT:       [[ARRAYIDX:%.*]] = getelementptr inbounds i32, i32* [[A:%.*]], i64 [[IDXPROM:%.*]]
; CHECK-NEXT:       %0 = load i32, i32* [[ARRAYIDX:%.*]], align 4
; CHECK-NEXT:       [[TOBOOL_NOT:%.*]] = icmp eq i32 %0, 0
; CHECK-NEXT:       br i1 [[TOBOOL_NOT:%.*]], label [[FOR_INC:%.*]], label [[IF_THEN:%.*]]
for.body:                                         ; preds = %for.cond
  %idxprom = zext i32 %i.0 to i64
  %arrayidx = getelementptr inbounds i32, i32* %a, i64 %idxprom
  %0 = load i32, i32* %arrayidx, align 4
  %tobool.not = icmp eq i32 %0, 0
  br i1 %tobool.not, label %for.inc, label %if.then

; CHECK:        if.then:
; CHECK-NEXT:       [[INC:%.*]] = add nsw i32 [[INC3:%.*]], 1
; CHECK-NEXT:       br label [[FOR_INC:%.*]]
if.then:                                          ; preds = %for.body
  %1 = load i32, i32* @u, align 4
  %inc = add nsw i32 %1, 1
  store i32 %inc, i32* @u, align 4
  br label %for.inc

; CHECK:        for.inc:
; CHECK-NEXT:       [[U_FLAG:%.*]] = phi i1 [ true, [[IF_THEN:%.*]] ], [ [[U_FLAG4:%.*]], [[FOR_COND:%.*]] ]
; CHECK-NEXT:       [[INC2:%.*]] = phi i32 [ [[INC:%.*]], [[IF_THEN:%.*]] ], [ [[INC3:%.*]], [[FOR_BODY:%.*]] ]
; CHECK-NEXT:       [[INC1:%.*]] = add nuw nsw i32 [[I_0:%.*]], 1
; CHECK-NEXT:       br label [[FOR_COND:%.*]]
for.inc:                                          ; preds = %for.body, %if.then
  %inc1 = add nuw nsw i32 %i.0, 1
  br label %for.cond

; CHECK:        for.cond.cleanup:
; CHECK-NEXT:       [[U_FLAG4_LCSSA:%.*]] = phi i1 [ [[U_FLAG4:%.*]], [[FOR_COND:%.*]] ]
; CHECK-NEXT:       [[INC3_LCSSA:%.*]] = phi i32 [ [[INC3:%.*]], [[FOR_COND:%.*]] ]
; CHECK-NEXT:       [[TOBOOL_U_FLAG:%.*]] = icmp eq i1 [[U_FLAG4_LCSSA:%.*]], true
; CHECK-NEXT:       br i1 [[TOBOOL_U_FLAG:%.*]], label [[U_FLAG_THEN_BB:%.*]], label [[U_FLAG_ELSE_BB:%.*]]
for.cond.cleanup:                                 ; preds = %for.cond
  ret void

; CHECK:        u.flag.then.bb:
; CHECK-NEXT:       store i32 [[INC3_LCSSA:%.*]], i32* @u, align 4
; CHECK-NEXT:       br label [[U_FLAG_ELSE_BB:%.*]]

; CHECK:        u.flag.else.bb:
; CHECK-NEXT:       ret void
}
