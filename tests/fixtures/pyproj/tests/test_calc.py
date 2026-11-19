import pytest
from calc import add, name
def test_add_small(): assert add(1, 2) == 3
@pytest.mark.parametrize("a", [6, 7, 8, 9])
def test_add_big(a): assert add(a, 1) == a + 1
def test_name(): assert name({"nm": "x"}) == "x"
def test_ok2(): assert True
