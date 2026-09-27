all: c s ss
c:c.c
	gcc c.c -o c
s:s.c
	gcc s.c -o s
ss:ss.c
	gcc ss.c -o ss -lpthread
clean:
	rm -rf cde-package cde.options c s ss
p:
	python printdb.py
