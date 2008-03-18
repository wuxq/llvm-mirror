; RUN: llvm-as < %s | opt -scalarrepl | llvm-dis | \
; RUN:   grep alloca | grep {4 x}

; Test that an array is not incorrectly deconstructed...

define i32 @test() {
	%X = alloca [4 x i32]		; <[4 x i32]*> [#uses=1]
	%Y = getelementptr [4 x i32]* %X, i64 0, i64 0		; <i32*> [#uses=1]
        ; Must preserve arrayness!
	%Z = getelementptr i32* %Y, i64 1		; <i32*> [#uses=1]
	%A = load i32* %Z		; <i32> [#uses=1]
	ret i32 %A
}
