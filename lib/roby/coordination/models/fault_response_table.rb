module Roby
    module Coordination
        module Models
            # Definition of the metamodel for Coordination::FaultResponseTable
            module FaultResponseTable
                include Roby::Actions::Models::Interface
                include Arguments

                # The set of defined fault handlers
                #
                # @return [FaultHandler]
                inherited_attribute('fault_handler', 'fault_handlers') { Array.new }

                def find_all_matching_handlers(exception)
                    each_fault_handler.find_all do |h|
                        h.execution_exception_matcher === exception
                    end
                end

                def on_fault(exception_matcher, &block)
                    exception_matcher = exception_matcher.to_execution_exception_matcher
                    @current_description = Roby::Actions::Models::Action.new
                    each_argument do |_, arg|
                        @current_description.required_arg(arg.name)
                    end
                    action_model, handler =
                        action_coordination(nil, Coordination::FaultHandler, &block)
                    handler.execution_exception_matcher(exception_matcher)
                    fault_handlers << handler
                    handler
                end

                def each_task
                    return enum_for(:each_task) if !block_given?
                    super
                    each_fault_handler do |handler|
                        if task = handler.replacement
                            yield(task)
                        end
                    end
                end

                def method_missing(m, *args, &block)
                    if Queries::ExecutionExceptionMatcher.method_defined?(m)
                        matcher = Queries::ExecutionExceptionMatcher.new
                        matcher.send(m, *args, &block)
                        matcher
                    else super
                    end
                end
            end
        end
    end
end

