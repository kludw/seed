; An error stops the program and says where it happened.
(define (safe-div a b)
  (if (= b 0)
      (error "cannot divide" a "by zero")
      (quotient a b)))
(display (safe-div 10 2)) (newline)
(display (safe-div 1 0)) (newline)
(display "not reached")
