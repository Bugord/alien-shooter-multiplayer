.PHONY: asmp-dll test build test clean format help

asmp-dll:
	$(MAKE) -C asmp-dll $(filter-out $@,$(MAKECMDGOALS))

test:
	$(MAKE) -C test $(filter-out $@,$(MAKECMDGOALS))

all:
	$(MAKE) -C asmp-dll $(filter-out $@,$(MAKECMDGOALS))
	$(MAKE) -C test $(filter-out $@,$(MAKECMDGOALS))

format:
	find . -type f \( -name '*.c' -o -name '*.cpp' -o -name '*.h' -o -name '*.hpp' \) \
		| xargs clang-format -i

help:
	@echo "This is the global Makefile for all components of the project."
	@echo "Usage: make [COMPONENT] [TARGET] [VARIABLES]"
	@echo ""
	@echo "Components:"
	@echo "  asmp-dll - client (asmp.dll)"
	@echo "  test     - tests"
	@echo "  all      - all components"
	@echo ""
	@echo "Targets:"
	@echo "  build  - build a component of the project"
	@echo "  clean  - remove data created during component building"
	@echo "  format - run clang-format on all source files"
	@echo "  help   - display this help message"
	@echo ""
	@echo "Variables:"
	@echo "  Avaliable variables are component-specific. Call 'help' for"
	@echo "  a specific component for more information."
