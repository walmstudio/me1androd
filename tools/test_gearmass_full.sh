#!/bin/bash
# Полное тестирование GEARMASS: unit-тесты + smoke tests + базовые проверки
# Использование: ./tools/test_gearmass_full.sh [--verbose]

set -e

VERBOSE=0
if [[ "$1" == "--verbose" ]] || [[ "$1" == "-v" ]]; then
    VERBOSE=1
fi

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

TESTS_PASSED=0
TESTS_FAILED=0
TESTS_TOTAL=0

log_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

log_success() {
    echo -e "${GREEN}[PASS]${NC} $1"
    ((TESTS_PASSED++))
}

log_fail() {
    echo -e "${RED}[FAIL]${NC} $1"
    ((TESTS_FAILED++))
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

run_test() {
    local name="$1"
    local cmd="$2"
    ((TESTS_TOTAL++))
    
    log_info "Running: $name"
    
    if [[ $VERBOSE -eq 1 ]]; then
        if eval "$cmd"; then
            log_success "$name"
        else
            log_fail "$name"
        fi
    else
        if eval "$cmd" > /tmp/gearmass_test_$$.log 2>&1; then
            log_success "$name"
        else
            log_fail "$name"
            echo "  Output saved to: /tmp/gearmass_test_$$.log"
        fi
    fi
}

# Header
echo "=================================="
echo "  GEARMASS Full Test Suite"
echo "=================================="
echo ""

# Check that we're in the project root
if [[ ! -f "Makefile" ]] || [[ ! -d "runtime" ]]; then
    log_fail "Must be run from project root directory"
    exit 1
fi

# 1. Unit tests
echo ""
log_info "=== Phase 1: Unit Tests ==="
echo ""

run_test "test_gm (pool, presets, policy)" \
    "make -s build/test_gm && ./build/test_gm"

run_test "test_gm_thermal (thermal sensors)" \
    "make -s build/test_gm_thermal && ./build/test_gm_thermal"

run_test "test_gm_resolution (auto-resolution)" \
    "make -s build/test_gm_resolution && ./build/test_gm_resolution"

run_test "test_gm_integration (end-to-end)" \
    "make -s build/test_gm_integration && ./build/test_gm_integration"

# 2. Runtime checks
echo ""
log_info "=== Phase 2: Runtime Component Checks ==="
echo ""

# Check gearmass.c for potential issues
run_test "gearmass.c compiles without warnings" \
    "gcc -O2 -Wall -Wextra -Werror -Wno-unused-parameter -Iruntime -c runtime/gearmass.c -o /tmp/gearmass_check_$$.o && rm /tmp/gearmass_check_$$.o"

# Check for TODO/FIXME comments
if grep -n "TODO\|FIXME\|XXX\|HACK" runtime/gearmass.c runtime/gearmass.h 2>/dev/null | grep -v "test_gm"; then
    log_warn "Found TODO/FIXME comments in GEARMASS code"
else
    log_success "No pending TODO/FIXME in GEARMASS code"
fi

# 3. Environment variable parsing
echo ""
log_info "=== Phase 3: Environment Variable Parsing ==="
echo ""

# Test basic mode parsing
run_test "ME1_GEARMASS=eco parsing" \
    "ME1_GEARMASS=eco ME1_GM_LOG=0 build/test_gm 2>&1 | grep -q 'ECO'"

run_test "ME1_GEARMASS=auto parsing" \
    "ME1_GEARMASS=auto ME1_GM_LOG=0 build/test_gm 2>&1 | grep -q 'AUTO'"

run_test "ME1_GEARMASS=max parsing" \
    "ME1_GEARMASS=max ME1_GM_LOG=0 build/test_gm 2>&1 | grep -q 'MAX'"

# 4. Documentation checks
echo ""
log_info "=== Phase 4: Documentation Checks ==="
echo ""

# Check that all modes are documented
for mode in ECO BALANCE PERF MAX CUSTOM AUTO OFF; do
    if grep -q "$mode" docs/PLAN_GEARMASS_DEVICE_TESTING.md; then
        log_success "Mode $mode documented"
        ((TESTS_PASSED++))
        ((TESTS_TOTAL++))
    else
        log_fail "Mode $mode not documented"
        ((TESTS_FAILED++))
        ((TESTS_TOTAL++))
    fi
done

# Check that GUIDE.md mentions GEARMASS
if grep -q "GEARMASS" docs/GUIDE.md 2>/dev/null || grep -q "Нагрузка" docs/GUIDE.md 2>/dev/null; then
    log_success "GEARMASS documented in GUIDE.md"
    ((TESTS_PASSED++))
else
    log_warn "GEARMASS might need more documentation in GUIDE.md"
fi
((TESTS_TOTAL++))

# 5. Code quality checks
echo ""
log_info "=== Phase 5: Code Quality Checks ==="
echo ""

# Check for common issues
if grep -n "printf.*\\\\n" runtime/gearmass.c | grep -v "fprintf.*stderr"; then
    log_warn "Found printf with \\n (should use log functions)"
else
    log_success "No raw printf calls with \\n"
    ((TESTS_PASSED++))
fi
((TESTS_TOTAL++))

# Check for potential race conditions keywords
if grep -n "static.*=" runtime/gearmass.c | grep -v "static const" | grep -v "static.*\[\].*=" | grep -v "^static int.*=" | tail -n +2; then
    log_warn "Found non-const static variables (potential race conditions)"
else
    log_success "No suspicious static variables"
    ((TESTS_PASSED++))
fi
((TESTS_TOTAL++))

# 6. Build system checks
echo ""
log_info "=== Phase 6: Build System Integration ==="
echo ""

# Check that Makefile has gearmass dependencies
if grep -q "gearmass.c\|gearmass.h" Makefile; then
    log_success "Makefile includes gearmass dependencies"
    ((TESTS_PASSED++))
else
    log_fail "Makefile missing gearmass dependencies"
    ((TESTS_FAILED++))
fi
((TESTS_TOTAL++))

# Check that Android build includes gearmass
if grep -q "gearmass" android/build_apk.sh 2>/dev/null || grep -q "gearmass" android/jni/*.c 2>/dev/null; then
    log_success "Android build includes gearmass"
    ((TESTS_PASSED++))
else
    log_warn "Android build might not include gearmass"
fi
((TESTS_TOTAL++))

# 7. Summary
echo ""
echo "=================================="
echo "  Test Summary"
echo "=================================="
echo ""
echo "Total tests:  $TESTS_TOTAL"
echo -e "Passed:       ${GREEN}$TESTS_PASSED${NC}"
echo -e "Failed:       ${RED}$TESTS_FAILED${NC}"
echo ""

if [[ $TESTS_FAILED -eq 0 ]]; then
    echo -e "${GREEN}✓ All tests passed!${NC}"
    echo ""
    echo "Next steps:"
    echo "  1. Review docs/PLAN_GEARMASS_DEVICE_TESTING.md"
    echo "  2. Test on real Android device"
    echo "  3. Test on Linux desktop"
    echo "  4. Run stability tests (30 min, 2+ hours)"
    exit 0
else
    echo -e "${RED}✗ Some tests failed${NC}"
    echo ""
    echo "Please fix the failed tests before device testing."
    exit 1
fi
