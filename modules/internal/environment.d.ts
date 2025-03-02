// SPDX-License-Identifier: MIT OR LGPL-2.0-or-later
// SPDX-FileCopyrightText: 2020 Evan Welsh <contact@evanwelsh.com>

declare var moduleGlobalThis: Global;

declare var compileModule: CompileFunc;
declare var compileInternalModule: CompileFunc;
declare var getRegistry: (global: Global) => Map<string, Module>;
declare var getSourceMapRegistry:
    (global: Global) => Map<string, SourceMapConsumer>;
declare var loadResourceOrFile: (uri: string) => string;
declare var loadResourceOrFileAsync: (uri: string) => Promise<string>;
declare var parseURI: (uri: string) => Uri;
declare var resolveRelativeResourceOrFile:
    (uri: string, relativePath: string) => string | null;
declare var setGlobalModuleLoader:
    (global: Global, loader: InternalModuleLoader) => void;
declare var setModulePrivate: (module: Module, private: ModulePrivate) => void;
declare var uriExists: (uri: string) => boolean;
declare var atob: (text: string) => string;

/**
 * Use '__internal: never' to prevent any object from being type compatible with
 * Module because it is an internal type.
 */
declare type Module = { __internal: never };
declare type Global = typeof globalThis;
declare type SchemeHandler = {
    load(uri: Uri): [contents: string, internal: boolean];
    loadAsync(uri: Uri): Promise<[string] | [string, boolean]>;
};

declare type Query = { [key: string]: string | undefined };
declare type CompileFunc = (uri: string, source: string) => Module;
declare type ResolvedModule = [Module | null, string, string];

declare type Uri = {
    uri: string;
    uriWithQuery: string;
    scheme: string;
    host: string;
    path: string;
    query: Query;
};
