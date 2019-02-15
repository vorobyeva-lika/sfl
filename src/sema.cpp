#include "ast.hpp"

namespace sfl {

Value* FuncValue::call(const vector<unique_ptr<Value>>& args) const {
	if (lambda->arity() != args.size()) {
		throw RuntimeError("arity of function call violated");
	}
	State state(closure);
	for (int i = 0; i < lambda->arity(); ++i) {
		state.set(lambda->args.decls[i]->name, args[i]->clone());
	}
	return lambda->call(state);
}

FuncValue::FuncValue(const Lambda* l, const State& state) : lambda(l) {
	for (const auto& cv : lambda->closureVars) {
		closure.set(cv, state.get(cv)->clone());
	}
}
string FuncValue::dump() const {
	return lambda->dump();
}

void Prog::run(const vector<int>& input) {
	vector<Value*> args;
	for (int x : input) {
		args.push_back(new IntValue(x));
	}
	vector<unique_ptr<Value>> arr;
	arr.emplace_back(new ArrayValue(args));
	FuncValue func(lambda.get());
	unique_ptr<Value> ret(func.call(arr));
	if (ret) {
		cout << ret->dump() << endl;
	} else {
		cout << "nullptr" << endl;
	}
}

}

