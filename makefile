all:
	$(MAKE) -C c

run:
	$(MAKE) -C c run

debug:
	$(MAKE) -C c debug

clean:
	$(MAKE) -C c clean

.PHONY: all run debug clean
