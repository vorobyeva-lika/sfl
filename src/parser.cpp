#include <sstream>
#include <iostream>
#include "peglib.h"
#include "ast.hpp"

namespace sfl {

using namespace std;

const char* sfl_syntax =
	R"(
		# SFL grammar

		SOURCE      <- STAT_SEQ
		STATEMENT   <- STAT_ASSIGN / STAT_IF / '{' STAT_SEQ '}' / STAT_WHILE / STAT_PRINT /STAT_EXPR
		STAT_SEQ    <- COMMENT* STATEMENT COMMENT* (';' COMMENT* STATEMENT COMMENT*)* ';' ? COMMENT* 
		STAT_ASSIGN <- ID '=' EXPR / ID ':' TYPE '=' EXPR
		STAT_IF     <- 'if' COND 'then' STATEMENT 'else' STATEMENT
		STAT_WHILE  <- 'while' COND 'do' STATEMENT
		STAT_PRINT  <- 'print' EXPR
		STAT_EXPR   <- EXPR
		COND        <- EXPR COND_OP EXPR
		COND_OP     <- '<=' / '>=' / '<' / '>' / '=' 

		EXPR         <- EX_INT / EX_BINARY / EX_UNARY / EX_ARR_ACCESS  / EX_ARR_MAKE / EX_ARR_LEN / EX_FUN_CALL / EX_LAMBDA / EX_VAR
		EX_INT       <- < [0-9]+ >
		EX_VAR       <- ID
		EX_BINARY    <- '(' EXPR BINARY_OP EXPR ')'
		BINARY_OP    <- '+' / '*' / '-' / '/' / '%'
		EX_UNARY     <- UNARY_OP EXPR
		UNARY_OP     <- '-'
		EX_ARR_ACCESS<- '[' EXPR '[' EXPR ']]'
		EX_ARR_MAKE  <- '[' EXPR (',' EXPR )* ']'
		EX_ARR_LEN   <- '|' EXPR '|'
		EX_FUN_CALL  <- '(' EXPR '(' CALL_ARGS ? '))'
		CALL_ARGS    <- EXPR (',' EXPR)*
		EX_LAMBDA    <- '/\\' LAMBDA_ARGS ? '->' STATEMENT
		LAMBDA_ARGS  <- VAR_DECL (',' VAR_DECL)*
		VAR_DECL     <- ID ':' TYPE

		TYPE      <- TP_INT / TP_ARRAY / TP_FUNC
		TP_INT    <- 'int'
		TP_ARRAY  <- '[' TYPE ']'
		TP_FUNC   <- '(' TP_ARGS ? ')' '->' TYPE
		TP_ARGS   <- TYPE (',' TYPE)*

		ID          <- < [a-zA-Z] [a-zA-Z0-9_]* >

		~COMMENT    <- COMMENT_ML / COMMENT_SL
		~COMMENT_ML <- '/*' < (!'*/' .)* > '*/'
		~COMMENT_SL <- '//' < (![\n$] .)+ >

		%whitespace <- [ \t\r\n]*  
	)";

inline string line_col(pair<int, int> info) {
	return string("line: ") + to_string(info.first) + ", col: " + to_string(info.second);
}

struct Context {
	bool empty() const {
		return types.empty();
	}
	void push() {
		types.push_back(map<string, Type*>());
	}
	void pop() {
		types.pop_back();
	}
	void addDecl(const string& name, Type* type) {
		if (Type* tp = findType(name)) {
			if (!type->equal(tp)) {
				throw Error("variable " + name + " is declared with different type: " + type->dump() + ", original type was: " + tp->dump());
			}
		} else {
			types.back()[name] = type;
		}
	}
	void newDecl(const string& name, Type* type) {
		if (Type* tp = findType(name)) {
			throw Error("variable " + name + " is already declared");
		} else {
			types.back()[name] = type;
		}
	}
	Type* getType(const string& name) {
		if (Type* type = findType(name)) {
			return type;
		} else {
			//return nullptr;
			throw Error("variable " + name + " is not typed");
		}
	}
	Type* findType(const string& name) {
		for (const auto& m : types) {
			auto x = m.find(name);
			if (x != m.end()) {
				return x->second;
			}
		}
		return nullptr;
	}
private:
	vector<map<string, Type*>> types;
};

template<class T>
std::function<T* (const peg::SemanticValues&)> wrap_error(std::function<T* (const peg::SemanticValues&)> f) {
	return [f](const peg::SemanticValues& sv) {
		try {
			return f(sv);
		} catch (Error& err) {
			err.line = sv.line_info().first;
			err.col = sv.line_info().second;
			throw err;
		}
	};
}

template<class T>
std::function<T* (const peg::SemanticValues&, peg::any& ctx)> wrap_error(std::function<T* (const peg::SemanticValues&, peg::any& ctx)> f) {
	return [f](const peg::SemanticValues& sv, peg::any& ctx) {
		try {
			return f(sv, ctx);
		} catch (Error& err) {
			err.line = sv.line_info().first;
			err.col = sv.line_info().second;
			throw err;
		}
	};
}

peg::parser parser(const string& file) {
	peg::parser parser(sfl_syntax);
	if (!parser) {
		throw Error("Error in SPL grammar");
	}
	parser["ID"] = [](const peg::SemanticValues& sv) {
		return sv.token();
	};
	parser["TP_INT"] = [](const peg::SemanticValues& sv) {
		return new Int();
	};
	parser["TP_ARRAY"] = wrap_error<Array>([](const peg::SemanticValues& sv) {
			return new Array(sv[0].get<Type*>());
	});
	parser["TP_ARGS"] = [](const peg::SemanticValues& sv) {
		return sv.transform<Type*>();
	};
	parser["TP_FUNC"] = wrap_error<Func>([](const peg::SemanticValues& sv) {
		if (sv.size() == 1) {
			return new Func(sv[0].get<Type*>());
		} else {
			return new Func(sv[1].get<Type*>(), sv[0].get<vector<Type*>>());
		}
	});
	parser["TYPE"] = [](const peg::SemanticValues& sv) {
		switch (sv.choice()) {
		case 0: return static_cast<Type*>(sv[0].get<Int*>());
		case 1: return static_cast<Type*>(sv[0].get<Array*>());
		case 2: return static_cast<Type*>(sv[0].get<Func*>());
		default: throw Error("impossible choice in TYPE");
		};
	};
	parser["EX_INT"] = wrap_error<IntConst>([](const peg::SemanticValues& sv) {
		return new IntConst(stoi(sv.token()));
	});
	parser["EX_BINARY"] = wrap_error<BinOp>([](const peg::SemanticValues& sv) {
		return new BinOp(sv[0].get<Expr*>(), sv[2].get<Expr*>(), sv[1].get<BinOp::Kind>());
	});
	parser["BINARY_OP"] = [](const peg::SemanticValues& sv) {
		switch (sv.choice()) {
		case 0:  return BinOp::PLUS;
		case 1:  return BinOp::MULT;
		case 2:  return BinOp::SUB;
		case 3:  return BinOp::DIV;
		case 4:  return BinOp::RES;
		default: throw Error("impossible choice in BINARY_OP");
		}
	};
	parser["EX_UNARY"] = wrap_error<UnOp>([](const peg::SemanticValues& sv) {
		return new UnOp(sv[1].get<Expr*>(), sv[0].get<UnOp::Kind>());
	});
	parser["UNARY_OP"] = [](const peg::SemanticValues& sv) {
		return UnOp::MINUS;
	};
	parser["EX_ARR_ACCESS"] = wrap_error<ArrayAccess>([](const peg::SemanticValues& sv) {
		return new ArrayAccess(sv[0].get<Expr*>(), sv[1].get<Expr*>());
	});
	parser["EX_ARR_MAKE"] = wrap_error<ArrayMake>([](const peg::SemanticValues& sv) {
		return new ArrayMake(sv.transform<Expr*>());
	});
	parser["EX_ARR_LEN"] = wrap_error<ArrayLen>([](const peg::SemanticValues& sv) {
		return new ArrayLen(sv[0].get<Expr*>());
	});
	parser["EX_FUN_CALL"] = wrap_error<FunCall>([](const peg::SemanticValues& sv) {
		if (sv.size() == 1) {
			return new FunCall(sv[0].get<Expr*>());
		} else {
			return new FunCall(sv[0].get<Expr*>(), sv[1].get<vector<Expr*>>());
		}
	});
	parser["CALL_ARGS"] = [](const peg::SemanticValues& sv) {
		return sv.transform<Expr*>();
	};
	parser["LAMBDA_ARGS"].enter = [](const char* s, size_t n, peg::any& ctx) {
		ctx.get<Context*>()->push();
	};
	parser["EX_LAMBDA"] = wrap_error<Lambda>([](const peg::SemanticValues& sv, peg::any& ctx) {
		ctx.get<Context*>()->pop();
		if (sv.size() == 1) {
			return new Lambda(sv[0].get<Statement*>());
		} else {
			return new Lambda(sv[1].get<Statement*>(), sv[0].get<vector<VarDecl*>>());
		}
	});
	parser["VAR_DECL"] = wrap_error<VarDecl>([](const peg::SemanticValues& sv, peg::any& ctx) {
		string name = sv[0].get<string>();
		Type* type = sv[1].get<Type*>();
		ctx.get<Context*>()->newDecl(name, type);
		return new VarDecl(name, type);
	});
	parser["LAMBDA_ARGS"] = [](const peg::SemanticValues& sv) {
		return sv.transform<VarDecl*>();
	};
	parser["EX_VAR"] = wrap_error<VarAccess>([](const peg::SemanticValues& sv, peg::any& ctx) {
		string name = sv[0].get<string>();
		Type* type = ctx.get<Context*>()->getType(name);
		return new VarAccess(name, type);
	});
	// EXPR         <- EX_INT / EX_BINARY / EX_UNARY / EX_ARR_ACCESS  / EX_ARR_MAKE / EX_ARR_LEN / EX_FUN_CALL / EX_LAMBDA / EX_VAR
	parser["EXPR"] = [](const peg::SemanticValues& sv) {
		switch (sv.choice()) {
		case 0:  return static_cast<Expr*>(sv[0].get<IntConst*>());
		case 1:  return static_cast<Expr*>(sv[0].get<BinOp*>());
		case 2:  return static_cast<Expr*>(sv[0].get<UnOp*>());
		case 3:  return static_cast<Expr*>(sv[0].get<ArrayAccess*>());
		case 4:  return static_cast<Expr*>(sv[0].get<ArrayMake*>());
		case 5:  return static_cast<Expr*>(sv[0].get<ArrayLen*>());
		case 6:  return static_cast<Expr*>(sv[0].get<FunCall*>());
		case 7:  return static_cast<Expr*>(sv[0].get<Lambda*>());
		case 8:  return static_cast<Expr*>(sv[0].get<VarAccess*>());
		default: throw Error("impossible choice in EXPR");
		}
	};
	parser["COND"] = wrap_error<Cond>([](const peg::SemanticValues& sv) {
		return new Cond(sv[0].get<Expr*>(), sv[2].get<Expr*>(), sv[1].get<Cond::Kind>());
	});
	parser["COND_OP"] = [](const peg::SemanticValues& sv) {
		switch (sv.choice()) {
		case 0:  return Cond::LESSEQ;
		case 1:  return Cond::GREATEQ;
		case 2:  return Cond::LESS;
		case 3:  return Cond::GREAT;
		case 4:  return Cond::EQ;
		default: throw Error("impossible choice in COND_OP");
		}
	};
	parser["STAT_WHILE"] = wrap_error<While>([](const peg::SemanticValues& sv) {
		return new While(sv[0].get<Cond*>(), sv[1].get<Statement*>());
	});
	parser["STAT_IF"] = wrap_error<If>([](const peg::SemanticValues& sv) {
		return new If(sv[0].get<Cond*>(), sv[1].get<Statement*>(), sv[2].get<Statement*>());
	});
	parser["STAT_ASSIGN"] = wrap_error<Assign>([](const peg::SemanticValues& sv, peg::any& ctx) {
		string name = sv[0].get<string>();
		switch (sv.choice()) {
		case 0:  {
			Type* type = ctx.get<Context*>()->getType(name);
			return new Assign(name, sv[1].get<Expr*>(), type->clone());
		}
		case 1:  {
			Type* type = sv[1].get<Type*>();
			ctx.get<Context*>()->addDecl(name, type);
			return new Assign(name, sv[2].get<Expr*>(), type);
		}
		default: throw Error("impossible choice in STAT_ASSIGN");
		}
	});
	parser["STAT_SEQ"].enter = [](const char* s, size_t n, peg::any& ctx) {
		ctx.get<Context*>()->push();
	};
	parser["STAT_SEQ"] = wrap_error<Seq>([](const peg::SemanticValues& sv, peg::any& ctx) {
		ctx.get<Context*>()->pop();
		return new Seq(sv.transform<Statement*>());
	});
	parser["STAT_EXPR"] = wrap_error<StatExpr>([](const peg::SemanticValues& sv) {
		return new StatExpr(sv[0].get<Expr*>());
	});
	parser["STAT_PRINT"] = wrap_error<Print>([](const peg::SemanticValues& sv) {
		return new Print(sv[0].get<Expr*>());
	});
	parser["STATEMENT"] = [](const peg::SemanticValues& sv) {
		switch (sv.choice()) {
		case 0:  return static_cast<Statement*>(sv[0].get<Assign*>());
		case 1:  return static_cast<Statement*>(sv[0].get<If*>());
		case 2:  return static_cast<Statement*>(sv[0].get<Seq*>());
		case 3:  return static_cast<Statement*>(sv[0].get<While*>());
		case 4:  return static_cast<Statement*>(sv[0].get<Print*>());
		case 5:  return static_cast<Statement*>(sv[0].get<StatExpr*>());
		default: throw Error("impossible choice in STATEMENT");
		}
	};
	parser["SOURCE"].enter = [](const char* s, size_t n, peg::any& ctx) {
		ctx.get<Context*>()->push();
	};
	parser["SOURCE"] = wrap_error<Prog>([](const peg::SemanticValues& sv, peg::any& ctx) {
		ctx.get<Context*>()->pop();
		return new Prog(new Lambda(sv[0].get<Seq*>(), {new VarDecl("init", new Array(new Int()))}));
	});
	parser.log = [](size_t line, size_t col, const std::string& msg) {
		throw Error(msg, pair<int, int>(line, col));
	};
	return parser;
}

Prog* parse(const string& file, const string& src) {
	Prog* ret = nullptr;
	unique_ptr<Context> context(new Context());
	peg::any ctx(context.get());
	try {
		if (!parser(file).parse<Prog*>(src.c_str(), ctx, ret)) {
			throw Error("parsing of " + file + " failed");
		}
	} catch (Error& err) {
		err.file = file;
		throw err;
	}
	if (!context->empty()) {
		throw Error("type stack is not empty, " + file + " failed");
	}
	return ret;
}

}