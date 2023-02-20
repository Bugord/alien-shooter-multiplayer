build:
	$(MAKE) -C asmp-dll $(MAKECMDGOALS)
build_tests:
	$(MAKE) -C asmp-dll $(MAKECMDGOALS)
clean:
	$(MAKE) -C asmp-dll $(MAKECMDGOALS)
format:
	find . -type f \( -name '*.c' -o -name '*.cpp' -o -name '*.h' -o -name '*.hpp' \) \
		-not -path '*/test/*' | xargs clang-format -i
help:
	$(MAKE) -C asmp-dll $(MAKECMDGOALS)
