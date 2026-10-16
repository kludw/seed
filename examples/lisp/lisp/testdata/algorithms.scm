; A few classic algorithms, and loops written as tail calls.
(define (quicksort xs)
  (if (null? xs)
      '()
      (let ((p (car xs)) (rest (cdr xs)))
        (append (quicksort (filter (lambda (x) (< x p)) rest))
                (list p)
                (quicksort (filter (lambda (x) (>= x p)) rest))))))
(display (quicksort '(3 1 4 1 5 9 2 6 5 3 5))) (newline)

(define (range a b) (if (>= a b) '() (cons a (range (+ a 1) b))))
(define (prime? n)
  (define (try d) (cond ((> (* d d) n) #t) ((= 0 (remainder n d)) #f) (else (try (+ d 1)))))
  (and (> n 1) (try 2)))
(display (filter prime? (range 1 60))) (newline)

(define (fib n)
  (define (go a b k) (if (= k 0) a (go b (+ a b) (- k 1))))
  (go 0 1 n))
(display (fib 100)) (newline)

(define (hanoi n) (if (= n 0) 0 (+ 1 (* 2 (hanoi (- n 1))))))
(display (hanoi 64)) (newline)

; A loop of a million iterations runs in constant space.
(define (count-up i n acc) (if (= i n) acc (count-up (+ i 1) n (+ acc i))))
(display (count-up 0 1000000 0)) (newline)

; Mutual tail calls too.
(define (my-even? n) (if (= n 0) #t (my-odd? (- n 1))))
(define (my-odd? n) (if (= n 0) #f (my-even? (- n 1))))
(display (my-even? 100001)) (newline)

(define (gcd a b) (if (= b 0) a (gcd b (remainder a b))))
(display (fold gcd 0 '(84 126 210))) (newline)
