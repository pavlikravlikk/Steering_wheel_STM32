# Convenience targets (host only, no hardware).
#   make test     host C + Python tests
#   make gen      regenerate files from config/hardware_profile.toml
#   make check    everything: tests + all firmware configurations + scans
#   make status   project status (TBD list, blockers) -> docs/STATUS.md
.PHONY: test test-c test-py gen check status

test: test-c test-py

test-c:
	$(MAKE) -C tests

test-py:
	python3 -m unittest discover -s tools/tests -t tools

gen:
	python3 tools/gen_config.py

check:
	bash tools/check_all.sh

status:
	python3 tools/project_status.py --write-status
