#!/usr/bin/env node

import {
  createConnection,
  TextDocuments,
  DiagnosticSeverity,
  InitializeResult,
  ServerCapabilities,
  Position,
  Range,
  CompletionItem,
  CompletionItemKind,
  SymbolInformation,
  SymbolKind,
  Location,
} from "vscode-languageserver";
import { TextDocument } from "vscode-languageserver-textdocument";

// ============================================================================
// LEXER
// ============================================================================

enum TokenType {
  // Literals
  NUMBER = "NUMBER",
  STRING = "STRING",
  IDENTIFIER = "IDENTIFIER",
  
  // Keywords
  FN = "FN",
  RETURN = "RETURN",
  IF = "IF",
  ELSE = "ELSE",
  WHILE = "WHILE",
  FOR = "FOR",
  BREAK = "BREAK",
  CONTINUE = "CONTINUE",
  SWITCH = "SWITCH",
  CASE = "CASE",
  DEFAULT = "DEFAULT",
  VAR = "VAR",
  CONST = "CONST",
  STRUCT = "STRUCT",
  ENUM = "ENUM",
  DEFER = "DEFER",
  USE = "USE",
  PRIVATE = "PRIVATE",
  AS = "AS",
  
  // Types
  INT = "INT",
  FLOAT = "FLOAT",
  STRING_TYPE = "STRING_TYPE",
  BOOL = "BOOL",
  VOID = "VOID",
  NIL = "NIL",
  
  // Operators
  PLUS = "PLUS",
  MINUS = "MINUS",
  STAR = "STAR",
  SLASH = "SLASH",
  PERCENT = "PERCENT",
  ASSIGN = "ASSIGN",
  EQ = "EQ",
  NE = "NE",
  LT = "LT",
  LE = "LE",
  GT = "GT",
  GE = "GE",
  AND = "AND",
  OR = "OR",
  NOT = "NOT",
  AMPERSAND = "AMPERSAND",
  PIPE = "PIPE",
  CARET = "CARET",
  TILDE = "TILDE",
  LSHIFT = "LSHIFT",
  RSHIFT = "RSHIFT",
  PLUS_PLUS = "PLUS_PLUS",
  MINUS_MINUS = "MINUS_MINUS",
  QUESTION = "QUESTION",
  
  // Delimiters
  LPAREN = "LPAREN",
  RPAREN = "RPAREN",
  LBRACE = "LBRACE",
  RBRACE = "RBRACE",
  LBRACKET = "LBRACKET",
  RBRACKET = "RBRACKET",
  SEMICOLON = "SEMICOLON",
  COLON = "COLON",
  COMMA = "COMMA",
  DOT = "DOT",
  ARROW = "ARROW",
  
  // Special
  EOF = "EOF",
  ERROR = "ERROR"
}

interface Token {
  type: TokenType;
  text: string;
  line: number;
  column: number;
  length: number;
}

const KEYWORDS = new Map<string, TokenType>([
  ["fn", TokenType.FN],
  ["return", TokenType.RETURN],
  ["if", TokenType.IF],
  ["else", TokenType.ELSE],
  ["while", TokenType.WHILE],
  ["for", TokenType.FOR],
  ["break", TokenType.BREAK],
  ["continue", TokenType.CONTINUE],
  ["switch", TokenType.SWITCH],
  ["case", TokenType.CASE],
  ["default", TokenType.DEFAULT],
  ["var", TokenType.VAR],
  ["const", TokenType.CONST],
  ["struct", TokenType.STRUCT],
  ["enum", TokenType.ENUM],
  ["defer", TokenType.DEFER],
  ["use", TokenType.USE],
  ["private", TokenType.PRIVATE],
  ["as", TokenType.AS],
  ["int", TokenType.INT],
  ["int8", TokenType.INT],
  ["int16", TokenType.INT],
  ["int32", TokenType.INT],
  ["int64", TokenType.INT],
  ["uint", TokenType.INT],
  ["uint8", TokenType.INT],
  ["uint16", TokenType.INT],
  ["uint32", TokenType.INT],
  ["uint64", TokenType.INT],
  ["float", TokenType.FLOAT],
  ["float32", TokenType.FLOAT],
  ["float64", TokenType.FLOAT],
  ["double", TokenType.FLOAT],
  ["string", TokenType.STRING_TYPE],
  ["bool", TokenType.BOOL],
  ["char", TokenType.INT],
  ["byte", TokenType.INT],
  ["void", TokenType.VOID],
  ["nil", TokenType.NIL],
  ["true", TokenType.NIL],
  ["false", TokenType.NIL],
]);

class Lexer {
  private source: string;
  private position: number = 0;
  private line: number = 1;
  private column: number = 1;
  private tokens: Token[] = [];

  constructor(source: string) {
    this.source = source;
  }

  tokenize(): Token[] {
    while (this.position < this.source.length) {
      this.skipWhitespaceAndComments();
      
      if (this.position >= this.source.length) break;

      const char = this.source[this.position];
      const startLine = this.line;
      const startColumn = this.column;

      // Numbers
      if (/[0-9]/.test(char)) {
        this.scanNumber(startLine, startColumn);
        continue;
      }

      // Strings
      if (char === '"') {
        this.scanString(startLine, startColumn);
        continue;
      }

      // Identifiers and keywords
      if (/[a-zA-Z_]/.test(char)) {
        this.scanIdentifier(startLine, startColumn);
        continue;
      }

      // Operators and delimiters
      if (!this.scanOperator(startLine, startColumn)) {
        this.position++;
        this.column++;
      }
    }

    this.tokens.push({
      type: TokenType.EOF,
      text: "",
      line: this.line,
      column: this.column,
      length: 0
    });

    return this.tokens;
  }

  private scanNumber(line: number, column: number) {
    const start = this.position;
    
    while (this.position < this.source.length && /[0-9.]/.test(this.source[this.position])) {
      this.advance();
    }

    const text = this.source.substring(start, this.position);
    this.tokens.push({
      type: TokenType.NUMBER,
      text,
      line,
      column,
      length: text.length
    });
  }

  private scanString(line: number, column: number) {
    this.advance(); // Opening quote
    const start = this.position;

    while (this.position < this.source.length && this.source[this.position] !== '"') {
      if (this.source[this.position] === '\\') {
        this.advance();
      }
      this.advance();
    }

    const text = this.source.substring(start, this.position);
    this.advance(); // Closing quote

    this.tokens.push({
      type: TokenType.STRING,
      text,
      line,
      column,
      length: text.length + 2
    });
  }

  private scanIdentifier(line: number, column: number) {
    const start = this.position;

    while (this.position < this.source.length && /[a-zA-Z0-9_]/.test(this.source[this.position])) {
      this.advance();
    }

    const text = this.source.substring(start, this.position);
    const type = KEYWORDS.get(text) || TokenType.IDENTIFIER;

    this.tokens.push({
      type,
      text,
      line,
      column,
      length: text.length
    });
  }

  private scanOperator(line: number, column: number): boolean {
    const char = this.source[this.position];
    const next = this.peek();

    const twoCharOps: { [key: string]: TokenType } = {
      "==": TokenType.EQ,
      "!=": TokenType.NE,
      "<=": TokenType.LE,
      ">=": TokenType.GE,
      "&&": TokenType.AND,
      "||": TokenType.OR,
      "++": TokenType.PLUS_PLUS,
      "--": TokenType.MINUS_MINUS,
      "<<": TokenType.LSHIFT,
      ">>": TokenType.RSHIFT,
      "->": TokenType.ARROW,
    };

    const twoChar = char + next;
    if (twoCharOps[twoChar]) {
      const type = twoCharOps[twoChar];
      this.tokens.push({ type, text: twoChar, line, column, length: 2 });
      this.advance();
      this.advance();
      return true;
    }

    const oneCharOps: { [key: string]: TokenType } = {
      "+": TokenType.PLUS,
      "-": TokenType.MINUS,
      "*": TokenType.STAR,
      "/": TokenType.SLASH,
      "%": TokenType.PERCENT,
      "=": TokenType.ASSIGN,
      "<": TokenType.LT,
      ">": TokenType.GT,
      "!": TokenType.NOT,
      "&": TokenType.AMPERSAND,
      "|": TokenType.PIPE,
      "^": TokenType.CARET,
      "~": TokenType.TILDE,
      "?": TokenType.QUESTION,
      "(": TokenType.LPAREN,
      ")": TokenType.RPAREN,
      "{": TokenType.LBRACE,
      "}": TokenType.RBRACE,
      "[": TokenType.LBRACKET,
      "]": TokenType.RBRACKET,
      ";": TokenType.SEMICOLON,
      ":": TokenType.COLON,
      ",": TokenType.COMMA,
      ".": TokenType.DOT,
    };

    if (oneCharOps[char]) {
      const type = oneCharOps[char];
      this.tokens.push({ type, text: char, line, column, length: 1 });
      this.advance();
      return true;
    }

    return false;
  }

  private skipWhitespaceAndComments() {
    while (this.position < this.source.length) {
      const char = this.source[this.position];
      const next = this.peek();

      // Whitespace
      if (/\s/.test(char)) {
        this.advance();
        continue;
      }

      // Line comment
      if (char === "/" && next === "/") {
        while (this.position < this.source.length && this.source[this.position] !== "\n") {
          this.advance();
        }
        continue;
      }

      // Block comment
      if (char === "/" && next === "*") {
        this.advance();
        this.advance();
        while (this.position < this.source.length - 1) {
          if (this.source[this.position] === "*" && this.source[this.position + 1] === "/") {
            this.advance();
            this.advance();
            break;
          }
          this.advance();
        }
        continue;
      }

      break;
    }
  }

  private advance() {
    if (this.position < this.source.length) {
      if (this.source[this.position] === "\n") {
        this.line++;
        this.column = 1;
      } else {
        this.column++;
      }
      this.position++;
    }
  }

  private peek(offset: number = 0): string {
    const pos = this.position + offset;
    return pos < this.source.length ? this.source[pos] : "";
  }
}

// ============================================================================
// PARSER
// ============================================================================

interface ASTNode {
  type: string;
  line: number;
  column: number;
  name?: string;
  children?: ASTNode[];
}

interface Symbol {
  name: string;
  kind: "function" | "variable" | "type" | "module";
  line: number;
  column: number;
  documentation?: string;
  signature?: string;
}

class Parser {
  private tokens: Token[] = [];
  private position: number = 0;
  private currentModule: string = "";
  public symbols: Map<string, Symbol[]> = new Map();
  private moduleSymbols: Map<string, Symbol> = new Map();

  constructor(source: string) {
    const lexer = new Lexer(source);
    this.tokens = lexer.tokenize();
  }

  parse(): ASTNode | null {
    try {
      return this.parseProgram();
    } catch (e) {
      return null;
    }
  }

  private parseProgram(): ASTNode {
    const node: ASTNode = {
      type: "Program",
      line: 1,
      column: 1,
      children: []
    };

    while (!this.isAtEnd() && this.current().type !== TokenType.EOF) {
      const item = this.parseTopLevel();
      if (item) {
        node.children!.push(item);
      }
    }

    return node;
  }

  private parseTopLevel(): ASTNode | null {
    const token = this.current();

    if (token.type === TokenType.USE) {
      return this.parseUse();
    }

    if (token.type === TokenType.FN) {
      return this.parseFunction();
    }

    if (token.type === TokenType.VAR || token.type === TokenType.CONST) {
      return this.parseVariableDeclaration();
    }

    if (token.type === TokenType.STRUCT) {
      return this.parseStruct();
    }

    if (token.type === TokenType.ENUM) {
      return this.parseEnum();
    }

    this.advance();
    return null;
  }

  private parseUse(): ASTNode {
    const line = this.current().line;
    const column = this.current().column;
    this.consume(TokenType.USE);

    const moduleName = this.current().text;
    this.advance();

    let alias = moduleName;
    if (this.match(TokenType.AS)) {
      this.advance();
      alias = this.current().text;
      this.advance();
    }

    this.consume(TokenType.SEMICOLON);

    this.registerSymbol(moduleName, "module", line, column);

    return {
      type: "Use",
      name: moduleName,
      line,
      column
    };
  }

  private parseFunction(): ASTNode {
    const line = this.current().line;
    const column = this.current().column;

    if (this.peekToken(-1)?.type === TokenType.PRIVATE) {
      this.position--;
      this.advance();
    }

    this.consume(TokenType.FN);

    const name = this.current().text;
    this.advance();

    // Generic parameters
    if (this.match(TokenType.LT)) {
      this.skipUntil(TokenType.GT);
    }

    this.consume(TokenType.LPAREN);

    const params: string[] = [];
    while (!this.check(TokenType.RPAREN) && !this.isAtEnd()) {
      if (this.current().type === TokenType.IDENTIFIER) {
        params.push(this.current().text);
      }
      this.advance();
    }

    this.consume(TokenType.RPAREN);

    // Return type
    let returnType = "void";
    if (this.match(TokenType.ARROW)) {
      this.advance();
      returnType = this.current().text || "auto";
      this.advance();
    }

    const signature = `fn ${name}(${params.join(", ")}) -> ${returnType}`;
    this.registerSymbol(name, "function", line, column);

    if (this.check(TokenType.LBRACE)) {
      this.skipBlock();
    } else {
      this.consume(TokenType.SEMICOLON);
    }

    return {
      type: "Function",
      name,
      line,
      column
    };
  }

  private parseVariableDeclaration(): ASTNode {
    const line = this.current().line;
    const column = this.current().column;
    const isConst = this.current().type === TokenType.CONST;
    this.advance();

    const name = this.current().text;
    this.advance();

    if (this.match(TokenType.COLON)) {
      this.advance();
      if (this.check(TokenType.LBRACKET)) {
        this.advance();
        this.advance();
      }
    }

    if (this.match(TokenType.ASSIGN)) {
      this.skipExpression();
    }

    this.consume(TokenType.SEMICOLON);

    this.registerSymbol(name, "variable", line, column);

    return {
      type: isConst ? "ConstDeclaration" : "VarDeclaration",
      name,
      line,
      column
    };
  }

  private parseStruct(): ASTNode {
    const line = this.current().line;
    const column = this.current().column;
    this.consume(TokenType.STRUCT);

    const name = this.current().text;
    this.advance();

    if (this.match(TokenType.LT)) {
      this.skipUntil(TokenType.GT);
    }

    this.consume(TokenType.LBRACE);

    const fields: string[] = [];
    while (!this.check(TokenType.RBRACE) && !this.isAtEnd()) {
      if (this.current().type === TokenType.IDENTIFIER) {
        fields.push(this.current().text);
      }
      this.advance();
    }

    this.consume(TokenType.RBRACE);

    this.registerSymbol(name, "type", line, column);

    return {
      type: "Struct",
      name,
      line,
      column
    };
  }

  private parseEnum(): ASTNode {
    const line = this.current().line;
    const column = this.current().column;
    this.consume(TokenType.ENUM);

    const name = this.current().text;
    this.advance();

    this.consume(TokenType.LBRACE);

    const members: string[] = [];
    while (!this.check(TokenType.RBRACE) && !this.isAtEnd()) {
      if (this.current().type === TokenType.IDENTIFIER) {
        members.push(this.current().text);
      }
      this.advance();
    }

    this.consume(TokenType.RBRACE);

    this.registerSymbol(name, "type", line, column);

    return {
      type: "Enum",
      name,
      line,
      column
    };
  }

  private skipExpression() {
    let depth = 0;
    while (!this.isAtEnd()) {
      if (this.check(TokenType.LPAREN) || this.check(TokenType.LBRACKET)) {
        depth++;
      } else if (this.check(TokenType.RPAREN) || this.check(TokenType.RBRACKET)) {
        depth--;
        if (depth < 0) break;
      } else if (depth === 0 && (this.check(TokenType.SEMICOLON) || this.check(TokenType.COMMA))) {
        break;
      }
      this.advance();
    }
  }

  private skipBlock() {
    this.consume(TokenType.LBRACE);
    let depth = 1;

    while (depth > 0 && !this.isAtEnd()) {
      if (this.check(TokenType.LBRACE)) depth++;
      else if (this.check(TokenType.RBRACE)) depth--;
      this.advance();
    }
  }

  private skipUntil(tokenType: TokenType) {
    while (!this.check(tokenType) && !this.isAtEnd()) {
      if (this.check(TokenType.LT)) {
        let depth = 1;
        this.advance();
        while (depth > 0 && !this.isAtEnd()) {
          if (this.check(TokenType.LT)) depth++;
          else if (this.check(TokenType.GT)) depth--;
          this.advance();
        }
        return;
      }
      this.advance();
    }
    if (this.check(tokenType)) this.advance();
  }

  private registerSymbol(name: string, kind: "function" | "variable" | "type" | "module", line: number, column: number) {
    const fullName = this.currentModule ? `${this.currentModule}.${name}` : name;
    
    if (!this.symbols.has(fullName)) {
      this.symbols.set(fullName, []);
    }

    this.symbols.get(fullName)!.push({
      name,
      kind,
      line,
      column
    });

    this.moduleSymbols.set(fullName, {
      name,
      kind,
      line,
      column
    });
  }

  public getSymbols(): Symbol[] {
    return Array.from(this.moduleSymbols.values());
  }

  public getSymbolAt(line: number, column: number): Symbol | null {
    for (const symbol of this.moduleSymbols.values()) {
      if (symbol.line === line && symbol.column <= column && column < symbol.column + symbol.name.length) {
        return symbol;
      }
    }
    return null;
  }

  private current(): Token {
    return this.tokens[this.position] || { type: TokenType.EOF, text: "", line: 0, column: 0, length: 0 };
  }

  private peek(): Token {
    return this.tokens[this.position + 1] || { type: TokenType.EOF, text: "", line: 0, column: 0, length: 0 };
  }

  private peekToken(offset: number): Token | null {
    const pos = this.position + offset;
    return pos >= 0 && pos < this.tokens.length ? this.tokens[pos] : null;
  }

  private advance() {
    if (!this.isAtEnd()) this.position++;
  }

  private check(type: TokenType): boolean {
    return this.current().type === type;
  }

  private match(...types: TokenType[]): boolean {
    for (const type of types) {
      if (this.check(type)) return true;
    }
    return false;
  }

  private consume(type: TokenType): Token {
    const token = this.current();
    if (token.type === type) {
      this.advance();
      return token;
    }
    throw new Error(`Expected ${type}, got ${token.type}`);
  }

  private isAtEnd(): boolean {
    return this.position >= this.tokens.length - 1;
  }
}

// ============================================================================
// LSP SERVER
// ============================================================================

const PROTON_KEYWORDS = [
  "fn", "return", "if", "else", "while", "for", "break", "continue",
  "switch", "case", "default", "var", "const", "struct", "enum", "defer",
  "use", "private", "as"
];

const PROTON_TYPES = [
  "int", "int8", "int16", "int32", "int64", "uint", "uint8", "uint16", "uint32", "uint64",
  "float", "float32", "float64", "double", "string", "bool", "char", "byte", "void", "nil"
];

const PROTON_BUILTINS = [
  "len", "push", "listCopy", "io.out", "io.in", "println", "print",
  "panic", "assert", "fs_read", "fs_write", "fs_exists", "sys_exec"
];

const connection = createConnection();
const documents = new TextDocuments(TextDocument);

interface DocumentInfo {
  parser: Parser;
  symbols: Symbol[];
}

const documentCache = new Map<string, DocumentInfo>();

documents.onDidOpen((event) => {
  parseDocument(event.document);
});

documents.onDidChange((event) => {
  parseDocument(event.document);
});

documents.onDidClose((event) => {
  documentCache.delete(event.document.uri);
});

function parseDocument(document: TextDocument) {
  const parser = new Parser(document.getText());
  const ast = parser.parse();
  
  documentCache.set(document.uri, {
    parser,
    symbols: parser.getSymbols()
  });

  const diagnostics = validateDocument(document);
  connection.sendDiagnostics({ uri: document.uri, diagnostics });
}

function validateDocument(document: TextDocument) {
  const diagnostics: any[] = [];
  const text = document.getText();
  const lexer = new Lexer(text);
  const tokens = lexer.tokenize();

  let braceStack: any[] = [];
  for (const token of tokens) {
    if (token.type === TokenType.LBRACE || token.type === TokenType.LPAREN || token.type === TokenType.LBRACKET) {
      braceStack.push(token);
    } else if (token.type === TokenType.RBRACE || token.type === TokenType.RPAREN || token.type === TokenType.RBRACKET) {
      if (braceStack.length === 0) {
        diagnostics.push({
          severity: DiagnosticSeverity.Error,
          range: Range.create(token.line - 1, token.column - 1, token.line - 1, token.column),
          message: `Unexpected closing ${token.text}`
        });
      } else {
        const last = braceStack.pop();
        const matching: any = {
          [TokenType.LBRACE]: TokenType.RBRACE,
          [TokenType.LPAREN]: TokenType.RPAREN,
          [TokenType.LBRACKET]: TokenType.RBRACKET
        };
        
        if (matching[last.type] !== token.type) {
          diagnostics.push({
            severity: DiagnosticSeverity.Error,
            range: Range.create(token.line - 1, token.column - 1, token.line - 1, token.column),
            message: `Mismatched brace: expected ${matching[last.type]}`
          });
        }
      }
    }
  }

  if (braceStack.length > 0) {
    const unclosed = braceStack[braceStack.length - 1];
    diagnostics.push({
      severity: DiagnosticSeverity.Error,
      range: Range.create(unclosed.line - 1, unclosed.column - 1, unclosed.line - 1, unclosed.column),
      message: `Unclosed ${unclosed.text}`
    });
  }

  return diagnostics;
}

connection.onInitialize((params): InitializeResult => {
  return {
    capabilities: {
      textDocumentSync: documents.syncKind,
      completionProvider: {
        resolveProvider: false,
        triggerCharacters: [".", ":", " "]
      },
      hoverProvider: true,
      definitionProvider: true,
      referencesProvider: true,
      documentSymbolProvider: true,
      workspaceSymbolProvider: true,
      renameProvider: true,
    } as any
  };
});

connection.onCompletion((params): CompletionItem[] => {
  const document = documents.get(params.textDocument.uri);
  if (!document) return [];

  const completions: CompletionItem[] = [];

  PROTON_KEYWORDS.forEach(keyword => {
    completions.push({
      label: keyword,
      kind: CompletionItemKind.Keyword,
      detail: "Proton keyword"
    });
  });

  PROTON_TYPES.forEach(type => {
    completions.push({
      label: type,
      kind: CompletionItemKind.TypeParameter,
      detail: "Proton type"
    });
  });

  PROTON_BUILTINS.forEach(builtin => {
    completions.push({
      label: builtin,
      kind: CompletionItemKind.Function,
      detail: "Proton built-in"
    });
  });

  const docInfo = documentCache.get(document.uri);
  if (docInfo) {
    docInfo.symbols.forEach(symbol => {
      const kind = symbol.kind === "function" ? CompletionItemKind.Function :
                  symbol.kind === "variable" ? CompletionItemKind.Variable :
                  symbol.kind === "type" ? CompletionItemKind.Class :
                  CompletionItemKind.Module;
      
      completions.push({
        label: symbol.name,
        kind,
        detail: `${symbol.kind} at line ${symbol.line}`
      });
    });
  }

  return completions;
});

connection.onHover((params) => {
  const document = documents.get(params.textDocument.uri);
  if (!document) return null;

  const docInfo = documentCache.get(document.uri);
  if (!docInfo) return null;

  const { line, character } = params.position;
  const symbol = docInfo.parser.getSymbolAt(line + 1, character + 1);

  if (symbol) {
    return {
      contents: `**${symbol.kind}:** ${symbol.name}\n\nLine: ${symbol.line}`
    };
  }

  return null;
});

connection.onDocumentSymbol((params) => {
  const document = documents.get(params.textDocument.uri);
  if (!document) return [];

  const docInfo = documentCache.get(document.uri);
  if (!docInfo) return [];

  return docInfo.symbols.map(symbol => ({
    name: symbol.name,
    kind: symbol.kind === "function" ? SymbolKind.Function :
           symbol.kind === "variable" ? SymbolKind.Variable :
           symbol.kind === "type" ? SymbolKind.Class :
           SymbolKind.Module,
    location: Location.create(
      document.uri,
      Range.create(symbol.line - 1, symbol.column - 1, symbol.line - 1, symbol.column + symbol.name.length)
    )
  }));
});

connection.onWorkspaceSymbol((params) => {
  const allSymbols: SymbolInformation[] = [];

  documentCache.forEach((docInfo, uri) => {
    docInfo.symbols.forEach(symbol => {
      if (symbol.name.includes(params.query)) {
        allSymbols.push({
          name: symbol.name,
          kind: symbol.kind === "function" ? SymbolKind.Function :
                 symbol.kind === "variable" ? SymbolKind.Variable :
                 symbol.kind === "type" ? SymbolKind.Class :
                 SymbolKind.Module,
          location: Location.create(
            uri,
            Range.create(symbol.line - 1, symbol.column - 1, symbol.line - 1, symbol.column + symbol.name.length)
          )
        });
      }
    });
  });

  return allSymbols;
});

connection.onRenameRequest((params) => {
  const document = documents.get(params.textDocument.uri);
  if (!document) return null;

  const docInfo = documentCache.get(document.uri);
  if (!docInfo) return null;

  const { line, character } = params.position;
  const symbol = docInfo.parser.getSymbolAt(line + 1, character + 1);

  if (!symbol) return null;

  const edits: any[] = [];
  const text = document.getText();
  const regex = new RegExp(`\\b${symbol.name}\\b`, "g");
  let match;

  while ((match = regex.exec(text)) !== null) {
    const start = document.positionAt(match.index);
    const end = document.positionAt(match.index + symbol.name.length);
    
    edits.push({
      range: Range.create(start, end),
      newText: params.newName
    });
  }

  return {
    changes: {
      [document.uri]: edits
    }
  };
});

documents.listen(connection);
connection.listen();
