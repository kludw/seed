; Closures keep their environment; set! changes the variable they share.
(define (make-counter)
  (let ((n 0))
    (lambda () (set! n (+ n 1)) n)))
(define a (make-counter))
(define b (make-counter))
(a) (a) (b)
(display (list (a) (b))) (newline)

(define (make-account balance)
  (define (deposit n) (set! balance (+ balance n)) balance)
  (define (withdraw n)
    (if (> n balance) (error "insufficient funds:" balance) (begin (set! balance (- balance n)) balance)))
  (lambda (op n) (if (eq? op 'deposit) (deposit n) (withdraw n))))
(define acc (make-account 100))
(acc 'deposit 50)
(display (acc 'withdraw 30)) (newline)

(define (compose f g) (lambda (x) (f (g x))))
(define add1 (lambda (x) (+ x 1)))
(display ((compose add1 add1) 5)) (newline)
(display (map (compose add1 (lambda (x) (* x x))) '(1 2 3))) (newline)

(define (curry f x) (lambda args (apply f (cons x args))))
(display ((curry + 1) 2 3 4)) (newline)

; Lexical scope: the x a closure sees is the one where it was made.
(define x 'global)
(define (get-x) x)
(let ((x 'local)) (display (list x (get-x))) (newline))

(letrec ((even? (lambda (n) (if (= n 0) #t (odd? (- n 1)))))
         (odd? (lambda (n) (if (= n 0) #f (even? (- n 1))))))
  (display (list (even? 10) (odd? 7))) (newline))
(display make-counter) (newline)
(display (procedure? car)) (newline)
